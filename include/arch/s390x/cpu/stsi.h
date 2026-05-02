// SPDX-License-Identifier: Apache-2.0
// include/arch/s390x/cpu/stsi.h

#ifndef ZXFOUNDATION_S390X_STSI_H
#define ZXFOUNDATION_S390X_STSI_H

#include <zxfoundation/types.h>

struct sysinfo_1_1_1 {
	unsigned char p:1;
	unsigned char :6;
	unsigned char t:1;
	unsigned char :8;
	unsigned char ccr;
	unsigned char cai;
	char reserved_0[20];
	unsigned long lic;
	char manufacturer[16];
	char type[4];
	char reserved_1[12];
	char model_capacity[16];
	char sequence[16];
	char plant[4];
	char model[16];
	char model_perm_cap[16];
	char model_temp_cap[16];
	unsigned int model_cap_rating;
	unsigned int model_perm_cap_rating;
	unsigned int model_temp_cap_rating;
	unsigned char typepct[5];
	unsigned char reserved_2[3];
	unsigned int ncr;
	unsigned int npr;
	unsigned int ntr;
	char reserved_3[4];
	char model_var_cap[16];
	unsigned int model_var_cap_rating;
	unsigned int nvr;
};

struct sysinfo_1_2_1 {
	char reserved_0[80];
	char sequence[16];
	char plant[4];
	char reserved_1[2];
	unsigned short cpu_address;
};

struct sysinfo_1_2_2 {
	char format;
	char reserved_0[1];
	unsigned short acc_offset;
	unsigned char mt_installed :1;
	unsigned char :2;
	unsigned char mt_stid :5;
	unsigned char :3;
	unsigned char mt_gtid :5;
	char reserved_1[18];
	unsigned int nominal_cap;
	unsigned int secondary_cap;
	unsigned int capability;
	unsigned short cpus_total;
	unsigned short cpus_configured;
	unsigned short cpus_standby;
	unsigned short cpus_reserved;
	unsigned short adjustment[];
};

struct sysinfo_1_2_2_extension {
	unsigned int alt_capability;
	unsigned short alt_adjustment[];
};

struct sysinfo_2_2_1 {
	char reserved_0[80];
	char sequence[16];
	char plant[4];
	unsigned short cpu_id;
	unsigned short cpu_address;
};

struct sysinfo_2_2_2 {
	char reserved_0[32];
	unsigned short lpar_number;
	char reserved_1;
	unsigned char characteristics;
	unsigned short cpus_total;
	unsigned short cpus_configured;
	unsigned short cpus_standby;
	unsigned short cpus_reserved;
	char name[8];
	unsigned int caf;
	char reserved_2[8];
	unsigned char mt_installed :1;
	unsigned char :2;
	unsigned char mt_stid :5;
	unsigned char :3;
	unsigned char mt_gtid :5;
	unsigned char :3;
	unsigned char mt_psmtid :5;
	char reserved_3[5];
	unsigned short cpus_dedicated;
	unsigned short cpus_shared;
	char reserved_4[3];
	unsigned char vsne;
	uuid_t uuid;
	char reserved_5[160];
	char ext_name[256];
};

#define LPAR_CHAR_DEDICATED	(1 << 7)
#define LPAR_CHAR_SHARED	(1 << 6)
#define LPAR_CHAR_LIMITED	(1 << 5)

struct sysinfo_3_2_2 {
	char reserved_0[31];
	unsigned char :4;
	unsigned char count:4;
	struct {
		char reserved_0[4];
		unsigned short cpus_total;
		unsigned short cpus_configured;
		unsigned short cpus_standby;
		unsigned short cpus_reserved;
		char name[8];
		unsigned int caf;
		char cpi[16];
		char reserved_1[3];
		unsigned char evmne;
		unsigned int reserved_2;
		uuid_t uuid;
	} vm[8];
	char reserved_3[1504];
	char ext_names[8][256];
};

/// @brief Issue STSI. Returns 0 on success, -1 if not supported.
/// @param sysinfo  4KB-aligned output block.
/// @param fc       Function code (e.g. 1).
/// @param sel1     Selector 1 (e.g. 1).
/// @param sel2     Selector 2 (e.g. 1).
int stsi(void *sysinfo, int fc, int sel1, int sel2);

#endif /* ZXFOUNDATION_S390X_STSI_H */
