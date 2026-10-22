/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _Q6_CVP_H
#define _Q6_CVP_H

#include "q6voice.h"

struct q6voice_session;

/* Format of the AFE port a vocproc is connected to */
struct q6cvp_dev_fmt {
	u16 port_id;
	u16 channels;
	u16 bits;
	u32 rate;
};

struct q6voice_session *q6cvp_session_create(enum q6voice_path_type path,
					     u16 tx_port, u16 rx_port);
struct q6voice_session *q6cvp_session_create_v3(enum q6voice_path_type path,
						const struct q6cvp_dev_fmt *tx,
						const struct q6cvp_dev_fmt *rx);
int q6cvp_enable(struct q6voice_session *cvp, bool enable);

#endif /*_Q6_CVP_H */
