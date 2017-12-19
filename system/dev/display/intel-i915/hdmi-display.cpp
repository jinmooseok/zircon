// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/driver.h>

#include "hdmi-display.h"
#include "macros.h"
#include "registers.h"
#include "registers-ddi.h"
#include "registers-dpll.h"
#include "registers-pipe.h"
#include "registers-transcoder.h"

// I2c functions

namespace {

int ddi_to_pin(registers::Ddi ddi) {
    if (ddi == registers::DDI_B) {
        return registers::GMBus0::kDdiBPin;
    } else if (ddi == registers::DDI_C) {
        return registers::GMBus0::kDdiCPin;
    } else if (ddi == registers::DDI_D) {
        return registers::GMBus0::kDdiDPin;
    }
    return -1;
}

void write_gmbus3(hwreg::RegisterIo* mmio_space, uint8_t* buf, uint32_t size, uint32_t idx) {
    int cur_byte = 0;
    uint32_t val = 0;
    while (idx < size && cur_byte < 4) {
        val |= buf[idx++] << (8 * cur_byte++);
    }
    registers::GMBus3::Get().FromValue(val).WriteTo(mmio_space);
}

void read_gmbus3(hwreg::RegisterIo* mmio_space, uint8_t* buf, uint32_t size, uint32_t idx) {
    int cur_byte = 0;
    uint32_t val = registers::GMBus3::Get().ReadFrom(mmio_space).reg_value();
    while (idx < size && cur_byte++ < 4) {
        buf[idx++] = val & 0xff;
        val >>= 8;
    }
}

} // namespace

namespace i915 {

HdmiDisplay::HdmiDisplay(Controller* controller, registers::Ddi ddi, registers::Pipe pipe)
        : DisplayDevice(controller, ddi, pipe) { }

bool HdmiDisplay::I2cRead(uint32_t addr, uint8_t* buf, uint32_t size) {
    return I2cTransfer(addr, buf, size, true /* read */, true /* allow_retry */);
}

bool HdmiDisplay::I2cWrite(uint32_t addr, uint8_t* buf, uint32_t size) {
    return I2cTransfer(addr, buf, size, false /* read */, true /* allow_retry */);
}

bool HdmiDisplay::I2cTransfer(const uint32_t addr, uint8_t* buf,
                              const uint32_t size, bool read, bool allow_retry) {
    // Reset the GMBus I2C port
    auto gmbus1 = registers::GMBus1::Get().FromValue(0);
    gmbus1.set_sw_clear_int(1);
    gmbus1.WriteTo(mmio_space());
    gmbus1.set_sw_clear_int(0);
    gmbus1.WriteTo(mmio_space());

    // Set the transfer pin
    auto gmbus0 = registers::GMBus0::Get().FromValue(0);
    gmbus0.set_pin_pair_select(ddi_to_pin(ddi()));
    gmbus0.WriteTo(mmio_space());

    // Disable interrupts
    auto gmbus4 = registers::GMBus4::Get().FromValue(0);
    gmbus4.set_interrupt_mask(0);
    gmbus4.WriteTo(mmio_space());

    unsigned idx = 0;
    if (!read) {
        write_gmbus3(mmio_space(), buf, size, idx);
        idx += 4;
    }

    gmbus1.ReadFrom(mmio_space());
    gmbus1.set_sw_ready(1);
    gmbus1.set_bus_cycle_wait(1);
    gmbus1.set_total_byte_count(size);
    gmbus1.set_slave_register_index(addr);
    gmbus1.set_read_op(read);
    gmbus1.WriteTo(mmio_space());

    do {
        // Allow one retry
        if (!I2cWaitForHwReady()) {
            if (!I2cClearNack()) {
                return false;
            }
            if (allow_retry) {
                return I2cTransfer(addr, buf, size, read, false /* allow_retry */);
            } else {
                zxlogf(ERROR, "hdmi: GMBus i2c %s too many failures\n", read ? "read" : "write");
                return false;
            }
        }

        if (idx >=  size) {
            break;
        }

        if (!read) {
            write_gmbus3(mmio_space(), buf, size, idx);
        } else {
            read_gmbus3(mmio_space(), buf, size, idx);
        }
        idx += 4;
    } while (idx < size);

    return I2cFinish();
}

bool HdmiDisplay::I2cFinish() {
    auto gmbus1 = registers::GMBus1::Get().FromValue(0);
    gmbus1.set_bus_cycle_stop(1);
    gmbus1.set_sw_ready(1);
    gmbus1.WriteTo(mmio_space());

    bool idle = WAIT_ON_MS(!registers::GMBus2::Get().ReadFrom(mmio_space()).active(), 100);

    auto gmbus0 = registers::GMBus0::Get().FromValue(0);
    gmbus0.set_pin_pair_select(0);
    gmbus0.WriteTo(mmio_space());

    if (!idle) {
        zxlogf(ERROR, "hdmi: GMBus i2c failed to go idle\n");
    }
    return idle;
}

bool HdmiDisplay::I2cWaitForHwReady() {
    auto gmbus2 = registers::GMBus2::Get().FromValue(0);
    if (!WAIT_ON_MS({ gmbus2.ReadFrom(mmio_space()); gmbus2.nack() || gmbus2.hw_ready(); }, 50)) {
        zxlogf(ERROR, "hdmi: GMBus i2c wait for hwready timeout\n");
        return false;
    }
    if (gmbus2.nack()) {
        zxlogf(ERROR, "hdmi: GMBus i2c got nack\n");
        return false;
    }
    return true;
}

bool HdmiDisplay::I2cClearNack() {
    I2cFinish();

    if (!WAIT_ON_MS(!registers::GMBus2::Get().ReadFrom(mmio_space()).active(), 10)) {
        zxlogf(ERROR, "hdmi: GMBus i2c failed to clear active nack\n");
        return false;
    }

    // Set/clear sw clear int to reset the bus
    auto gmbus1 = registers::GMBus1::Get().FromValue(0);
    gmbus1.set_sw_clear_int(1);
    gmbus1.WriteTo(mmio_space());
    gmbus1.set_sw_clear_int(0);
    gmbus1.WriteTo(mmio_space());

    // Reset GMBus0
    auto gmbus0 = registers::GMBus0::Get().FromValue(0);
    gmbus0.WriteTo(mmio_space());

    return true;
}

} // namespace i915

// Modesetting functions

namespace {

// See the section on HDMI/DVI programming in intel-gfx-prm-osrc-skl-vol12-display.pdf
// for documentation on this algorithm.
static bool calculate_params(uint32_t symbol_clock_khz,
                             uint64_t* dco_freq_khz, uint32_t* dco_central_freq_khz,
                             uint8_t* p0, uint8_t* p1, uint8_t* p2) {
    uint8_t even_candidates[36] = {
        4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 28, 30, 32, 36, 40, 42,
        44, 48, 52, 54, 56, 60, 64, 66, 68, 70, 72, 76, 78, 80, 84, 88, 90, 92, 96, 98
    };
    uint8_t odd_candidates[7] = { 3, 5, 7, 9, 15, 21, 35 };
    uint32_t candidate_freqs[3] = { 8400000, 9000000, 9600000 };
    uint32_t chosen_central_freq = 0;
    uint8_t chosen_divisor = 0;
    uint64_t afe_clock = symbol_clock_khz * 5;
    uint32_t best_deviation = 60; // Deviation in .1% intervals

    for (int parity = 0; parity < 2; parity++) {
        uint8_t* candidates;
        uint8_t num_candidates;
        if (parity) {
            candidates = odd_candidates;
            num_candidates = sizeof(odd_candidates) / sizeof(*odd_candidates);
        } else {
            candidates = even_candidates;
            num_candidates = sizeof(even_candidates) / sizeof(*even_candidates);
        }

        for (unsigned i = 0; i < sizeof(candidate_freqs) / sizeof(*candidate_freqs); i++) {
            uint32_t candidate_freq = candidate_freqs[i];
            for (unsigned j = 0; j < num_candidates; j++) {
                uint8_t candidate_divisor = candidates[j];
                uint64_t dco_freq = candidate_divisor * afe_clock;
                if (dco_freq > candidate_freq) {
                    uint32_t deviation = static_cast<uint32_t>(
                            1000 * (dco_freq - candidate_freq) / candidate_freq);
                    // positive deviation must be < 1%
                    if (deviation < 10 && deviation < best_deviation) {
                        best_deviation = deviation;
                        chosen_central_freq = candidate_freq;
                        chosen_divisor = candidate_divisor;
                    }
                } else {
                    uint32_t deviation = static_cast<uint32_t>(
                            1000 * (candidate_freq - dco_freq) / candidate_freq);
                    if (deviation < best_deviation) {
                        best_deviation = deviation;
                        chosen_central_freq = candidate_freq;
                        chosen_divisor = candidate_divisor;
                    }
                }
            }
        }
        if (chosen_divisor) {
            break;
        }
    }
    if (!chosen_divisor) {
        return false;
    }
    *p0 = *p1 = *p2 = 1;
    if (chosen_divisor % 2 == 0) {
        uint8_t chosen_divisor1 = chosen_divisor / 2;
        if (chosen_divisor1 == 1 || chosen_divisor1 == 2
                || chosen_divisor1 == 3 || chosen_divisor1 == 5) {
            *p0 = 2;
            *p2 = chosen_divisor1;
        } else if (chosen_divisor1 % 2 == 0) {
            *p0 = 2;
            *p1 = chosen_divisor1 / 2;
            *p2 = 2;
        } else if (chosen_divisor1 % 3 == 0) {
            *p0 = 3;
            *p1 = chosen_divisor1 / 3;
            *p2 = 2;
        } else if (chosen_divisor1 % 7 == 0) {
            *p0 = 7;
            *p1 = chosen_divisor1 / 7;
            *p2 = 2;
        }
    } else if (chosen_divisor == 3 || chosen_divisor == 9) {
        *p0 = 3;
        *p2 = chosen_divisor / 3;
    } else if (chosen_divisor == 5 || chosen_divisor == 7) {
        *p0 = chosen_divisor;
    } else if (chosen_divisor == 15) {
        *p0 = 3;
        *p2 = 5;
    } else if (chosen_divisor == 21) {
        *p0 = 7;
        *p2 = 3;
    } else if (chosen_divisor == 35) {
        *p0 = 7;
        *p2 = 5;
    }
    *dco_freq_khz = chosen_divisor * afe_clock;
    *dco_central_freq_khz = chosen_central_freq;
    return true;
}

} // namespace

namespace i915 {

bool HdmiDisplay::Init(zx_display_info* info) {
    // HDMI isn't supported on these DDIs
    if (ddi_to_pin(ddi()) == -1) {
        return false;
    }

    if (!ResetPipe() || !ResetDdi()) {
        return false;
    }

    registers::BaseEdid edid;
    if (!LoadEdid(&edid) || !EnablePowerWell2()) {
        return false;
    }

    // Set the the DPLL control settings
    auto dpll_ctrl1 = registers::DpllControl1::Get().ReadFrom(mmio_space());
    dpll_ctrl1.dpll_hdmi_mode(dpll()).set(1);
    dpll_ctrl1.dpll_override(dpll()).set(1);
    dpll_ctrl1.dpll_ssc_enable(dpll()).set(0);
    dpll_ctrl1.WriteTo(mmio_space());
    dpll_ctrl1.ReadFrom(mmio_space());

    // Calculate and the HDMI DPLL parameters
    uint8_t p0, p1, p2;
    uint32_t dco_central_freq_khz;
    uint64_t dco_freq_khz;
    if (!calculate_params(edid.preferred_timing.pixel_clock_10khz * 10,
                          &dco_freq_khz, &dco_central_freq_khz, &p0, &p1, &p2)) {
        zxlogf(ERROR, "hdmi: failed to calculate clock params\n");
        return false;
    }

    // Set the DCO frequency
    auto dpll_cfg1 = registers::DpllConfig1::Get(dpll()).FromValue(0);
    uint16_t dco_int = static_cast<uint16_t>((dco_freq_khz / 1000) / 24);
    uint16_t dco_frac = static_cast<uint16_t>(
            ((dco_freq_khz * (1 << 15) / 24) - ((dco_int * 1000L) * (1 << 15))) / 1000);
    dpll_cfg1.set_frequency_enable(1);
    dpll_cfg1.set_dco_integer(dco_int);
    dpll_cfg1.set_dco_fraction(dco_frac);
    dpll_cfg1.WriteTo(mmio_space());
    dpll_cfg1.ReadFrom(mmio_space());

    // Set the divisors and central frequency
    auto dpll_cfg2 = registers::DpllConfig2::Get(dpll()).FromValue(0);
    dpll_cfg2.set_qdiv_ratio(p1);
    dpll_cfg2.set_qdiv_mode(p1 != 1);
    if (p2 == 5) {
        dpll_cfg2.set_kdiv_ratio(dpll_cfg2.kKdiv5);
    } else if (p2 == 2) {
        dpll_cfg2.set_kdiv_ratio(dpll_cfg2.kKdiv2);
    } else if (p2 == 3) {
        dpll_cfg2.set_kdiv_ratio(dpll_cfg2.kKdiv3);
    } else { // p2 == 1
        dpll_cfg2.set_kdiv_ratio(dpll_cfg2.kKdiv1);
    }
    if (p0 == 1) {
        dpll_cfg2.set_pdiv_ratio(dpll_cfg2.kPdiv1);
    } else if (p0 == 2) {
        dpll_cfg2.set_pdiv_ratio(dpll_cfg2.kPdiv2);
    } else if (p0 == 3) {
        dpll_cfg2.set_pdiv_ratio(dpll_cfg2.kPdiv3);
    } else { // p0 == 7
        dpll_cfg2.set_pdiv_ratio(dpll_cfg2.kPdiv7);
    }
    if (dco_central_freq_khz == 9600000) {
        dpll_cfg2.set_central_freq(dpll_cfg2.k9600Mhz);
    } else if (dco_central_freq_khz == 9000000) {
        dpll_cfg2.set_central_freq(dpll_cfg2.k9000Mhz);
    } else { // dco_central_freq == 8400000
        dpll_cfg2.set_central_freq(dpll_cfg2.k8400Mhz);
    }
    dpll_cfg2.WriteTo(mmio_space());
    dpll_cfg2.ReadFrom(mmio_space()); // Posting read

    // Enable and wait for the DPLL
    auto dpll_enable = registers::DpllEnable::Get(dpll()).ReadFrom(mmio_space());
    dpll_enable.set_enable_dpll(1);
    dpll_enable.WriteTo(mmio_space());
    if (!WAIT_ON_MS(registers::DpllStatus
            ::Get().ReadFrom(mmio_space()).dpll_lock(dpll()).get(), 5)) {
        zxlogf(ERROR, "hdmi: DPLL failed to lock\n");
        return false;
    }

    // Direct the DPLL to the DDI
    auto dpll_ctrl2 = registers::DpllControl2::Get().ReadFrom(mmio_space());
    dpll_ctrl2.ddi_select_override(ddi()).set(1);
    dpll_ctrl2.ddi_clock_off(ddi()).set(0);
    dpll_ctrl2.ddi_clock_select(ddi()).set(dpll());
    dpll_ctrl2.WriteTo(mmio_space());

    // Enable DDI IO power and wait for it
    auto pwc2 = registers::PowerWellControl2::Get().ReadFrom(mmio_space());
    pwc2.ddi_io_power_request(ddi()).set(1);
    pwc2.WriteTo(mmio_space());
    if (!WAIT_ON_US(registers::PowerWellControl2
            ::Get().ReadFrom(mmio_space()).ddi_io_power_state(ddi()).get(), 20)) {
        zxlogf(ERROR, "hdmi: failed to enable IO power for ddi\n");
        return false;
    }

    registers::TranscoderRegs trans_regs(pipe());

    // Configure Transcoder Clock Select
    auto trans_clk_sel = trans_regs.ClockSelect().ReadFrom(mmio_space());
    trans_clk_sel.set_trans_clock_select(ddi() + 1);
    trans_clk_sel.WriteTo(mmio_space());

    // Configure the transcoder
    uint32_t h_active = edid.preferred_timing.horizontal_addressable() - 1;
    uint32_t h_sync_start = h_active + edid.preferred_timing.horizontal_front_porch();
    uint32_t h_sync_end = h_sync_start + edid.preferred_timing.horizontal_sync_pulse_width();
    uint32_t h_total = h_active + edid.preferred_timing.horizontal_blanking();

    uint32_t v_active = edid.preferred_timing.vertical_addressable() - 1;
    uint32_t v_sync_start = v_active + edid.preferred_timing.vertical_front_porch();
    uint32_t v_sync_end = v_sync_start + edid.preferred_timing.vertical_sync_pulse_width();
    uint32_t v_total = v_active + edid.preferred_timing.vertical_blanking();

    auto h_total_reg = trans_regs.HTotal().FromValue(0);
    h_total_reg.set_count_total(h_total);
    h_total_reg.set_count_active(h_active);
    h_total_reg.WriteTo(mmio_space());
    auto v_total_reg = trans_regs.VTotal().FromValue(0);
    v_total_reg.set_count_total(v_total);
    v_total_reg.set_count_active(v_active);
    v_total_reg.WriteTo(mmio_space());

    auto h_sync_reg = trans_regs.HSync().FromValue(0);
    h_sync_reg.set_sync_start(h_sync_start);
    h_sync_reg.set_sync_end(h_sync_end);
    h_sync_reg.WriteTo(mmio_space());
    auto v_sync_reg = trans_regs.VSync().FromValue(0);
    v_sync_reg.set_sync_start(v_sync_start);
    v_sync_reg.set_sync_end(v_sync_end);
    v_sync_reg.WriteTo(mmio_space());

    // The Intel docs say that H/VBlank should be programmed with the same H/VTotal
    trans_regs.HBlank().FromValue(h_total_reg.reg_value()).WriteTo(mmio_space());
    trans_regs.VBlank().FromValue(v_total_reg.reg_value()).WriteTo(mmio_space());

    auto ddi_func = trans_regs.DdiFuncControl().ReadFrom(mmio_space());
    ddi_func.set_trans_ddi_function_enable(1);
    ddi_func.set_ddi_select(ddi());
    ddi_func.set_trans_ddi_mode_select(ddi_func.kModeHdmi);
    ddi_func.set_bits_per_color(ddi_func.k8bbc);
    ddi_func.set_sync_polarity(edid.preferred_timing.vsync_polarity() << 1
                                | edid.preferred_timing.hsync_polarity());
    ddi_func.set_port_sync_mode_enable(0);
    ddi_func.set_dp_vc_payload_allocate(0);
    ddi_func.WriteTo(mmio_space());

    auto trans_conf = trans_regs.Conf().ReadFrom(mmio_space());
    trans_conf.set_transcoder_enable(1);
    trans_conf.set_interlaced_mode(edid.preferred_timing.interlaced());
    trans_conf.WriteTo(mmio_space());

    // Configure voltage swing and related IO settings.
    // TODO(ZX-1413): Use different values for different hardware (hardcoded to NUC for now)
    registers::DdiRegs ddi_regs(ddi());
    auto ddi_buf_trans_hi = ddi_regs.DdiBufTransHi(9).ReadFrom(mmio_space());
    auto ddi_buf_trans_lo = ddi_regs.DdiBufTransLo(9).ReadFrom(mmio_space());
    auto disio_cr_tx_bmu = registers::DisplayIoCtrlRegTxBmu::Get().ReadFrom(mmio_space());

    ddi_buf_trans_hi.set_reg_value(0x000000cd);
    ddi_buf_trans_lo.set_reg_value(0x80003015);
    disio_cr_tx_bmu.set_disable_balance_leg(0);
    disio_cr_tx_bmu.tx_balance_leg_select(ddi()).set(1);

    ddi_buf_trans_hi.WriteTo(mmio_space());
    ddi_buf_trans_lo.WriteTo(mmio_space());
    disio_cr_tx_bmu.WriteTo(mmio_space());

    // Configure and enable DDI_BUF_CTL
    auto ddi_buf_ctl = ddi_regs.DdiBufControl().ReadFrom(mmio_space());
    ddi_buf_ctl.set_ddi_buffer_enable(1);
    ddi_buf_ctl.WriteTo(mmio_space());

    // Configure the pipe
    registers::PipeRegs pipe_regs(pipe());

    auto pipe_size = pipe_regs.PipeSourceSize().FromValue(0);
    pipe_size.set_horizontal_source_size(h_active);
    pipe_size.set_vertical_source_size(v_active);
    pipe_size.WriteTo(mmio_space());

    auto plane_control = pipe_regs.PlaneControl().FromValue(0);
    plane_control.set_plane_enable(1);
    plane_control.set_source_pixel_format(plane_control.kFormatRgb8888);
    plane_control.set_tiled_surface(plane_control.kLinear);
    plane_control.WriteTo(mmio_space());

    auto plane_size = pipe_regs.PlaneSurfaceSize().FromValue(0);
    plane_size.set_width_minus_1(h_active);
    plane_size.set_height_minus_1(v_active);
    plane_size.WriteTo(mmio_space());

    info->width = edid.preferred_timing.horizontal_addressable();
    info->height = edid.preferred_timing.vertical_addressable();
    info->stride = ROUNDUP(info->width, registers::PlaneSurfaceStride::kLinearStrideChunkSize);
    info->format = ZX_PIXEL_FORMAT_ARGB_8888;
    info->pixelsize = ZX_PIXEL_FORMAT_BYTES(info->format);

    return true;
}

} // namespace i915
