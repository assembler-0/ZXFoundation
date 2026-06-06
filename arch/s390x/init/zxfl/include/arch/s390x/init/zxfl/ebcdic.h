/// SPDX-License-Identifier: Apache-2.0
/// include/zxfoundation/ebcdic.h - ASCII/EBCDIC conversion tables

#pragma once

#include <zxfoundation/types.h>

uint8_t ascii_to_ebcdic(uint8_t ascii);
uint8_t ebcdic_to_ascii(uint8_t ebcdic);
void ascii_to_ebcdic_buf(char *buf, size_t len);
void ebcdic_to_ascii_buf(char *buf, size_t len);
