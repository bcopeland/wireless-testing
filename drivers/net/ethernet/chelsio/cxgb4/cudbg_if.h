/*
 *  Copyright (C) 2017 Chelsio Communications.  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms and conditions of the GNU General Public License,
 *  version 2, as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  The full GNU General Public License is included in this distribution in
 *  the file called "COPYING".
 *
 */

#ifndef __CUDBG_IF_H__
#define __CUDBG_IF_H__

/* Error codes */
#define CUDBG_STATUS_NO_MEM -19
#define CUDBG_STATUS_ENTITY_NOT_FOUND -24
#define CUDBG_SYSTEM_ERROR -29

#define CUDBG_MAJOR_VERSION 1
#define CUDBG_MINOR_VERSION 14

enum cudbg_dbg_entity_type {
	CUDBG_REG_DUMP = 1,
	CUDBG_DEV_LOG = 2,
	CUDBG_CIM_IBQ_TP0 = 6,
	CUDBG_CIM_IBQ_TP1 = 7,
	CUDBG_CIM_IBQ_ULP = 8,
	CUDBG_CIM_IBQ_SGE0 = 9,
	CUDBG_CIM_IBQ_SGE1 = 10,
	CUDBG_CIM_IBQ_NCSI = 11,
	CUDBG_CIM_OBQ_ULP0 = 12,
	CUDBG_CIM_OBQ_ULP1 = 13,
	CUDBG_CIM_OBQ_ULP2 = 14,
	CUDBG_CIM_OBQ_ULP3 = 15,
	CUDBG_CIM_OBQ_SGE = 16,
	CUDBG_CIM_OBQ_NCSI = 17,
	CUDBG_EDC0 = 18,
	CUDBG_EDC1 = 19,
	CUDBG_TP_INDIRECT = 36,
	CUDBG_SGE_INDIRECT = 37,
	CUDBG_CIM_OBQ_RXQ0 = 47,
	CUDBG_CIM_OBQ_RXQ1 = 48,
	CUDBG_PCIE_INDIRECT = 50,
	CUDBG_PM_INDIRECT = 51,
	CUDBG_MA_INDIRECT = 61,
	CUDBG_UP_CIM_INDIRECT = 64,
	CUDBG_MBOX_LOG = 66,
	CUDBG_HMA_INDIRECT = 67,
	CUDBG_MAX_ENTITY = 70,
};

struct cudbg_init {
	struct adapter *adap; /* Pointer to adapter structure */
	void *outbuf; /* Output buffer */
	u32 outbuf_size;  /* Output buffer size */
};

static inline unsigned int cudbg_mbytes_to_bytes(unsigned int size)
{
	return size * 1024 * 1024;
}
#endif /* __CUDBG_IF_H__ */
