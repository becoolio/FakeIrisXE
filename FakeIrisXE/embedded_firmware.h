//
//  embedded_firmware.h
//  FakeIrisXEFramebuffer
//
//  Created by Anomy on 03/12/25.
//
// embedded_firmware.h
#ifndef EMBEDDED_FIRMWARE_H
#define EMBEDDED_FIRMWARE_H

#ifdef __cplusplus
extern "C" {
#endif

// GuC Firmware for Tiger Lake
extern const unsigned char tgl_guc_70_1_1_bin[];
extern const unsigned int tgl_guc_70_1_1_bin_len;

// HuC Firmware for Tiger Lake
extern const unsigned char tgl_huc_7_9_3_bin[];
extern const unsigned int tgl_huc_7_9_3_bin_len;


// GuC Firmware for Tiger Lake (corrected from Alder Lake P - using same binary)
extern const unsigned char tgl_guc_70_1_1_bin[];
extern const unsigned int tgl_guc_70_1_1_bin_len;


// DMC Firmware for Tiger Lake
extern const unsigned char tgl_dmc_ver2_12_bin[];
extern const unsigned int tgl_dmc_ver2_12_bin_len;


// DMC Firmware for Alder Lake P
extern const unsigned char adlp_dmc_ver2_16_bin[];
extern const unsigned int adlp_dmc_ver2_16_bin_len;

// V142: Apple TGL GuC Firmware (61,888 bytes)
// From AppleIntelTGLGuC.framework/Resources/GuC.bin
extern const unsigned char apple_guc_bin[];
extern const unsigned int apple_guc_bin_len;

#ifdef __cplusplus
}
#endif

#endif // EMBEDDED_FIRMWARE_H
