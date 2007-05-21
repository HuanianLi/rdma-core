/*
 * Copyright (c) 2007 Cisco, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <netinet/in.h>

#include "mlx4.h"
#include "mlx4-abi.h"
#include "wqe.h"

int mlx4_query_device(struct ibv_context *context, struct ibv_device_attr *attr)
{
	struct ibv_query_device cmd;
	uint64_t raw_fw_ver;
	unsigned major, minor, sub_minor;
	int ret;

	ret = ibv_cmd_query_device(context, attr, &raw_fw_ver, &cmd, sizeof cmd);
	if (ret)
		return ret;

	major     = (raw_fw_ver >> 32) & 0xffff;
	minor     = (raw_fw_ver >> 16) & 0xffff;
	sub_minor = raw_fw_ver & 0xffff;

	snprintf(attr->fw_ver, sizeof attr->fw_ver,
		 "%d.%d.%03d", major, minor, sub_minor);

	return 0;
}

int mlx4_query_port(struct ibv_context *context, uint8_t port,
		     struct ibv_port_attr *attr)
{
	struct ibv_query_port cmd;

	return ibv_cmd_query_port(context, port, attr, &cmd, sizeof cmd);
}

struct ibv_pd *mlx4_alloc_pd(struct ibv_context *context)
{
	struct ibv_alloc_pd       cmd;
	struct mlx4_alloc_pd_resp resp;
	struct mlx4_pd		 *pd;

	pd = malloc(sizeof *pd);
	if (!pd)
		return NULL;

	if (ibv_cmd_alloc_pd(context, &pd->ibv_pd, &cmd, sizeof cmd,
			     &resp.ibv_resp, sizeof resp)) {
		free(pd);
		return NULL;
	}

	pd->pdn = resp.pdn;

	return &pd->ibv_pd;
}

int mlx4_free_pd(struct ibv_pd *pd)
{
	int ret;

	ret = ibv_cmd_dealloc_pd(pd);
	if (ret)
		return ret;

	free(to_mpd(pd));
	return 0;
}

struct ibv_mr *mlx4_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
			   enum ibv_access_flags access)
{
	struct ibv_mr *mr;
	struct ibv_reg_mr cmd;
	int ret;

	mr = malloc(sizeof *mr);
	if (!mr)
		return NULL;

#ifdef IBV_CMD_REG_MR_HAS_RESP_PARAMS
	{
		struct ibv_reg_mr_resp resp;

		ret = ibv_cmd_reg_mr(pd, addr, length, (uintptr_t) addr,
				     access, mr, &cmd, sizeof cmd,
				     &resp, sizeof resp);
	}
#else
	ret = ibv_cmd_reg_mr(pd, addr, length, (uintptr_t) addr, access, mr,
			     &cmd, sizeof cmd);
#endif
	if (ret) {
		free(mr);
		return NULL;
	}

	return mr;
}

int mlx4_dereg_mr(struct ibv_mr *mr)
{
	int ret;

	ret = ibv_cmd_dereg_mr(mr);
	if (ret)
		return ret;

	free(mr);
	return 0;
}

static int align_cq_size(int cqe)
{
	int nent;

	for (nent = 1; nent <= cqe; nent <<= 1)
		; /* nothing */

	return nent;
}

struct ibv_cq *mlx4_create_cq(struct ibv_context *context, int cqe,
			       struct ibv_comp_channel *channel,
			       int comp_vector)
{
	struct mlx4_create_cq      cmd;
	struct mlx4_create_cq_resp resp;
	struct mlx4_cq		  *cq;
	int			   ret;

	/* Sanity check CQ size before proceeding */
	if (cqe > 0x3fffff)
		return NULL;

	cq = malloc(sizeof *cq);
	if (!cq)
		return NULL;

	cq->cons_index = 0;

	if (pthread_spin_init(&cq->lock, PTHREAD_PROCESS_PRIVATE))
		goto err;

	cqe = align_cq_size(cqe);

	if (mlx4_alloc_buf(&cq->buf, cqe * MLX4_CQ_ENTRY_SIZE,
			   to_mdev(context->device)->page_size))
		goto err;

	memset(cq->buf.buf, 0, cqe * MLX4_CQ_ENTRY_SIZE);

	cq->set_ci_db  = mlx4_alloc_db(to_mctx(context), MLX4_DB_TYPE_CQ);
	if (!cq->set_ci_db)
		goto err_buf;

	cq->arm_db     = cq->set_ci_db + 1;
	*cq->arm_db    = 0;
	cq->arm_sn     = 1;
	*cq->set_ci_db = 0;

	cmd.buf_addr = (uintptr_t) cq->buf.buf;
	cmd.db_addr  = (uintptr_t) cq->set_ci_db;

	ret = ibv_cmd_create_cq(context, cqe - 1, channel, comp_vector,
				&cq->ibv_cq, &cmd.ibv_cmd, sizeof cmd,
				&resp.ibv_resp, sizeof resp);
	if (ret)
		goto err_db;

	cq->cqn = resp.cqn;

	return &cq->ibv_cq;

err_db:
	mlx4_free_db(to_mctx(context), cq->set_ci_db);

err_buf:
	mlx4_free_buf(&cq->buf);

err:
	free(cq);

	return NULL;
}

int mlx4_resize_cq(struct ibv_cq *ibcq, int cqe)
{
	/* XXX resize CQ not implemented */
	return -ENOSYS;
}

int mlx4_destroy_cq(struct ibv_cq *cq)
{
	int ret;

	ret = ibv_cmd_destroy_cq(cq);
	if (ret)
		return ret;

	mlx4_free_db(to_mctx(cq->context), to_mcq(cq)->set_ci_db);
	mlx4_free_buf(&to_mcq(cq)->buf);
	free(to_mcq(cq));

	return 0;
}

static int align_queue_size(struct ibv_context *context, int size, int spare)
{
	int ret;

	/*
	 * If someone asks for a 0-sized queue, presumably they're not
	 * going to use it.  So don't mess with their size.
	 */
	if (!size)
		return 0;

	for (ret = 1; ret < size + spare; ret <<= 1)
		; /* nothing */

	return ret;
}

struct ibv_srq *mlx4_create_srq(struct ibv_pd *pd,
				 struct ibv_srq_init_attr *attr)
{
	struct mlx4_create_srq      cmd;
	struct mlx4_create_srq_resp resp;
	struct mlx4_srq		   *srq;
	int			    ret;

	/* Sanity check SRQ size before proceeding */
	if (attr->attr.max_wr > 1 << 16 || attr->attr.max_sge > 64)
		return NULL;

	srq = malloc(sizeof *srq);
	if (!srq)
		return NULL;

	if (pthread_spin_init(&srq->lock, PTHREAD_PROCESS_PRIVATE))
		goto err;

	srq->max     = align_queue_size(pd->context, attr->attr.max_wr, 1);
	srq->max_gs  = attr->attr.max_sge;
	srq->counter = 0;

	if (mlx4_alloc_srq_buf(pd, &attr->attr, srq))
		goto err;

	srq->db = mlx4_alloc_db(to_mctx(pd->context), MLX4_DB_TYPE_RQ);
	if (!srq->db)
		goto err_free;

	*srq->db = 0;

	cmd.buf_addr = (uintptr_t) srq->buf.buf;
	cmd.db_addr  = (uintptr_t) srq->db;

	ret = ibv_cmd_create_srq(pd, &srq->ibv_srq, attr,
				 &cmd.ibv_cmd, sizeof cmd,
				 &resp.ibv_resp, sizeof resp);
	if (ret)
		goto err_db;

	srq->srqn = resp.srqn;

	return &srq->ibv_srq;

err_db:
	mlx4_free_db(to_mctx(pd->context), srq->db);

err_free:
	free(srq->wrid);
	mlx4_free_buf(&srq->buf);

err:
	free(srq);

	return NULL;
}

int mlx4_modify_srq(struct ibv_srq *srq,
		     struct ibv_srq_attr *attr,
		     enum ibv_srq_attr_mask attr_mask)
{
	struct ibv_modify_srq cmd;

	return ibv_cmd_modify_srq(srq, attr, attr_mask, &cmd, sizeof cmd);
}

int mlx4_query_srq(struct ibv_srq *srq,
		    struct ibv_srq_attr *attr)
{
	struct ibv_query_srq cmd;

	return ibv_cmd_query_srq(srq, attr, &cmd, sizeof cmd);
}

int mlx4_destroy_srq(struct ibv_srq *srq)
{
	int ret;

	ret = ibv_cmd_destroy_srq(srq);
	if (ret)
		return ret;

	mlx4_free_db(to_mctx(srq->context), to_msrq(srq)->db);
	mlx4_free_buf(&to_msrq(srq)->buf);
	free(to_msrq(srq)->wrid);
	free(to_msrq(srq));

	return 0;
}

struct ibv_qp *mlx4_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *attr)
{
	struct mlx4_create_qp     cmd;
	struct ibv_create_qp_resp resp;
	struct mlx4_qp		 *qp;
	int			  ret;

	/* Sanity check QP size before proceeding */
	if (attr->cap.max_send_wr     > 65536 ||
	    attr->cap.max_recv_wr     > 65536 ||
	    attr->cap.max_send_sge    > 64    ||
	    attr->cap.max_recv_sge    > 64    ||
	    attr->cap.max_inline_data > 1024)
		return NULL;

	qp = malloc(sizeof *qp);
	if (!qp)
		return NULL;

	qp->sq.max = align_queue_size(pd->context, attr->cap.max_send_wr, 0);
	qp->rq.max = align_queue_size(pd->context, attr->cap.max_recv_wr, 0);

	if (mlx4_alloc_qp_buf(pd, &attr->cap, attr->qp_type, qp))
		goto err;

	mlx4_init_qp_indices(qp);

	if (pthread_spin_init(&qp->sq.lock, PTHREAD_PROCESS_PRIVATE) ||
	    pthread_spin_init(&qp->rq.lock, PTHREAD_PROCESS_PRIVATE))
		goto err_free;

	qp->db = mlx4_alloc_db(to_mctx(pd->context), MLX4_DB_TYPE_RQ);
	if (!qp->db)
		goto err_free;

	*qp->db = 0;

	cmd.buf_addr	  = (uintptr_t) qp->buf.buf;
	cmd.db_addr	  = (uintptr_t) qp->db;
	cmd.log_sq_stride = qp->sq.wqe_shift;
	for (cmd.log_sq_bb_count = 0;
	     qp->sq.max > 1 << cmd.log_sq_bb_count;
	     ++cmd.log_sq_bb_count)
		; /* nothing */
	memset(cmd.reserved, 0, sizeof cmd.reserved);

	ret = ibv_cmd_create_qp(pd, &qp->ibv_qp, attr, &cmd.ibv_cmd, sizeof cmd,
				&resp, sizeof resp);
	if (ret)
		goto err_rq_db;

	ret = mlx4_store_qp(to_mctx(pd->context), qp->ibv_qp.qp_num, qp);
	if (ret)
		goto err_destroy;

	qp->rq.max    = attr->cap.max_recv_wr;
	qp->rq.max_gs = attr->cap.max_recv_sge;
	mlx4_set_sq_sizes(qp, &attr->cap, attr->qp_type);

	qp->doorbell_qpn    = htonl(qp->ibv_qp.qp_num << 8);
	if (attr->sq_sig_all)
		qp->sq_signal_bits = htonl(MLX4_WQE_CTRL_CQ_UPDATE);
	else
		qp->sq_signal_bits = 0;

	return &qp->ibv_qp;

err_destroy:
	ibv_cmd_destroy_qp(&qp->ibv_qp);

err_rq_db:
	mlx4_free_db(to_mctx(pd->context), qp->db);

err_free:
	free(qp->sq.wrid);
	free(qp->rq.wrid);
	mlx4_free_buf(&qp->buf);

err:
	free(qp);

	return NULL;
}

int mlx4_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
		   enum ibv_qp_attr_mask attr_mask,
		   struct ibv_qp_init_attr *init_attr)
{
	struct ibv_query_qp cmd;

	return ibv_cmd_query_qp(qp, attr, attr_mask, init_attr, &cmd, sizeof cmd);
}

int mlx4_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
		    enum ibv_qp_attr_mask attr_mask)
{
	struct ibv_modify_qp cmd;
	int ret;

	ret = ibv_cmd_modify_qp(qp, attr, attr_mask, &cmd, sizeof cmd);

	if (!ret		       &&
	    (attr_mask & IBV_QP_STATE) &&
	    attr->qp_state == IBV_QPS_RESET) {
		mlx4_cq_clean(to_mcq(qp->recv_cq), qp->qp_num,
			       qp->srq ? to_msrq(qp->srq) : NULL);
		if (qp->send_cq != qp->recv_cq)
			mlx4_cq_clean(to_mcq(qp->send_cq), qp->qp_num, NULL);

		mlx4_init_qp_indices(to_mqp(qp));
	}

	return ret;
}

static void mlx4_lock_cqs(struct ibv_qp *qp)
{
	struct mlx4_cq *send_cq = to_mcq(qp->send_cq);
	struct mlx4_cq *recv_cq = to_mcq(qp->recv_cq);

	if (send_cq == recv_cq)
		pthread_spin_lock(&send_cq->lock);
	else if (send_cq->cqn < recv_cq->cqn) {
		pthread_spin_lock(&send_cq->lock);
		pthread_spin_lock(&recv_cq->lock);
	} else {
		pthread_spin_lock(&recv_cq->lock);
		pthread_spin_lock(&send_cq->lock);
	}
}

static void mlx4_unlock_cqs(struct ibv_qp *qp)
{
	struct mlx4_cq *send_cq = to_mcq(qp->send_cq);
	struct mlx4_cq *recv_cq = to_mcq(qp->recv_cq);

	if (send_cq == recv_cq)
		pthread_spin_unlock(&send_cq->lock);
	else if (send_cq->cqn < recv_cq->cqn) {
		pthread_spin_unlock(&recv_cq->lock);
		pthread_spin_unlock(&send_cq->lock);
	} else {
		pthread_spin_unlock(&send_cq->lock);
		pthread_spin_unlock(&recv_cq->lock);
	}
}

int mlx4_destroy_qp(struct ibv_qp *ibqp)
{
	struct mlx4_qp *qp = to_mqp(ibqp);
	int ret;

	mlx4_cq_clean(to_mcq(ibqp->recv_cq), ibqp->qp_num,
		       ibqp->srq ? to_msrq(ibqp->srq) : NULL);
	if (ibqp->send_cq != ibqp->recv_cq)
		mlx4_cq_clean(to_mcq(ibqp->send_cq), ibqp->qp_num, NULL);

	mlx4_lock_cqs(ibqp);
	mlx4_clear_qp(to_mctx(ibqp->context), ibqp->qp_num);
	mlx4_unlock_cqs(ibqp);

	ret = ibv_cmd_destroy_qp(ibqp);
	if (ret) {
		mlx4_lock_cqs(ibqp);
		mlx4_store_qp(to_mctx(ibqp->context), ibqp->qp_num, qp);
		mlx4_unlock_cqs(ibqp);

		return ret;
	}

	mlx4_free_db(to_mctx(ibqp->context), qp->db);
	free(qp->sq.wrid);
	free(qp->rq.wrid);
	mlx4_free_buf(&qp->buf);
	free(qp);

	return 0;
}

struct ibv_ah *mlx4_create_ah(struct ibv_pd *pd, struct ibv_ah_attr *attr)
{
	struct mlx4_ah *ah;

	ah = malloc(sizeof *ah);
	if (!ah)
		return NULL;

	memset(&ah->av, 0, sizeof ah->av);

	ah->av.port_pd   = htonl(to_mpd(pd)->pdn | (attr->port_num << 24));
	ah->av.g_slid    = attr->src_path_bits;
	ah->av.dlid      = htons(attr->dlid);
	if (attr->static_rate) {
		ah->av.stat_rate = attr->static_rate + MLX4_STAT_RATE_OFFSET;
		/* XXX check rate cap? */
	}
	ah->av.sl_tclass_flowlabel = htonl(attr->sl << 28);
	if (attr->is_global) {
		ah->av.g_slid   |= 0x80;
		ah->av.gid_index = attr->grh.sgid_index;
		ah->av.hop_limit = attr->grh.hop_limit;
		ah->av.sl_tclass_flowlabel |=
			htonl((attr->grh.traffic_class << 20) |
				    attr->grh.flow_label);
		memcpy(ah->av.dgid, attr->grh.dgid.raw, 16);
	}

	return &ah->ibv_ah;
}

int mlx4_destroy_ah(struct ibv_ah *ah)
{
	free(to_mah(ah));

	return 0;
}

int mlx4_attach_mcast(struct ibv_qp *qp, union ibv_gid *gid, uint16_t lid)
{
	return ibv_cmd_attach_mcast(qp, gid, lid);
}

int mlx4_detach_mcast(struct ibv_qp *qp, union ibv_gid *gid, uint16_t lid)
{
	return ibv_cmd_detach_mcast(qp, gid, lid);
}
