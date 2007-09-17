/**************************************************************************
Etherboot -  BOOTP/TFTP Bootstrap Program
Skeleton NIC driver for Etherboot
***************************************************************************/

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 */

#include <errno.h>
#include <gpxe/pci.h>
#include <gpxe/malloc.h>
#include <gpxe/iobuf.h>
#include <gpxe/netdevice.h>
#include <gpxe/infiniband.h>
#include <gpxe/ipoib.h>

/* to get some global routines like printf */
#include "etherboot.h"
/* to get the interface to the body of the program */
#include "nic.h"

#define CREATE_OWN 1

#include "mt25218_imp.c"

#include "arbel.h"


struct ib_address_vector hack_ipoib_bcast_av;








/***************************************************************************
 *
 * Queue number allocation
 *
 ***************************************************************************
 */

/**
 * Allocate queue number
 *
 * @v q_inuse		Queue usage bitmask
 * @v max_inuse		Maximum number of in-use queues
 * @ret qn_offset	Free queue number offset, or negative error
 */
static int arbel_alloc_qn_offset ( arbel_bitmask_t *q_inuse,
				   unsigned int max_inuse ) {
	unsigned int qn_offset = 0;
	arbel_bitmask_t mask = 1;

	while ( qn_offset < max_inuse ) {
		if ( ( mask & *q_inuse ) == 0 ) {
			*q_inuse |= mask;
			return qn_offset;
		}
		qn_offset++;
		mask <<= 1;
		if ( ! mask ) {
			mask = 1;
			q_inuse++;
		}
	}
	return -ENFILE;
}

/**
 * Free queue number
 *
 * @v q_inuse		Queue usage bitmask
 * @v qn_offset		Queue number offset
 */
static void arbel_free_qn_offset ( arbel_bitmask_t *q_inuse, int qn_offset ) {
	arbel_bitmask_t mask;

	mask = ( 1 << ( qn_offset % ( 8 * sizeof ( mask ) ) ) );
	q_inuse += ( qn_offset / ( 8 * sizeof ( mask ) ) );
	*q_inuse &= ~mask;
}

/***************************************************************************
 *
 * HCA commands
 *
 ***************************************************************************
 */

/**
 * Wait for Arbel command completion
 *
 * @v arbel		Arbel device
 * @ret rc		Return status code
 */
static int arbel_cmd_wait ( struct arbel *arbel,
			    struct arbelprm_hca_command_register *hcr ) {
	unsigned int wait;

	for ( wait = ARBEL_HCR_MAX_WAIT_MS ; wait ; wait-- ) {
		hcr->u.dwords[6] =
			readl ( arbel->config + ARBEL_HCR_REG ( 6 ) );
		if ( MLX_GET ( hcr, go ) == 0 )
			return 0;
		mdelay ( 1 );
	}
	return -EBUSY;
}

/**
 * Issue HCA command
 *
 * @v arbel		Arbel device
 * @v command		Command opcode, flags and input/output lengths
 * @v op_mod		Opcode modifier (0 if no modifier applicable)
 * @v in		Input parameters
 * @v in_mod		Input modifier (0 if no modifier applicable)
 * @v out		Output parameters
 * @ret rc		Return status code
 */
static int arbel_cmd ( struct arbel *arbel, unsigned long command,
		       unsigned int op_mod, const void *in,
		       unsigned int in_mod, void *out ) {
	struct arbelprm_hca_command_register hcr;
	unsigned int opcode = ARBEL_HCR_OPCODE ( command );
	size_t in_len = ARBEL_HCR_IN_LEN ( command );
	size_t out_len = ARBEL_HCR_OUT_LEN ( command );
	void *in_buffer;
	void *out_buffer;
	unsigned int status;
	unsigned int i;
	int rc;

	DBGC2 ( arbel, "Arbel %p command %02x in %zx%s out %zx%s\n",
		arbel, opcode, in_len,
		( ( command & ARBEL_HCR_IN_MBOX ) ? "(mbox)" : "" ), out_len,
		( ( command & ARBEL_HCR_OUT_MBOX ) ? "(mbox)" : "" ) );

	/* Check that HCR is free */
	if ( ( rc = arbel_cmd_wait ( arbel, &hcr ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p command interface locked\n", arbel );
		return rc;
	}

	/* Prepare HCR */
	memset ( &hcr, 0, sizeof ( hcr ) );
	in_buffer = &hcr.u.dwords[0];
	if ( in_len && ( command & ARBEL_HCR_IN_MBOX ) ) {
		in_buffer = arbel->mailbox_in;
		MLX_FILL_1 ( &hcr, 1, in_param_l, virt_to_bus ( in_buffer ) );
	}
	memcpy ( in_buffer, in, in_len );
	MLX_FILL_1 ( &hcr, 2, input_modifier, in_mod );
	out_buffer = &hcr.u.dwords[3];
	if ( out_len && ( command & ARBEL_HCR_OUT_MBOX ) ) {
		out_buffer = arbel->mailbox_out;
		MLX_FILL_1 ( &hcr, 4, out_param_l,
			     virt_to_bus ( out_buffer ) );
	}
	MLX_FILL_3 ( &hcr, 6,
		     opcode, opcode,
		     opcode_modifier, op_mod,
		     go, 1 );
	DBGC2_HD ( arbel, &hcr, sizeof ( hcr ) );
	if ( in_len ) {
		DBGC2 ( arbel, "Input:\n" );
		DBGC2_HD ( arbel, in, ( ( in_len < 256 ) ? in_len : 256 ) );
	}

	/* Issue command */
	for ( i = 0 ; i < ( sizeof ( hcr ) / sizeof ( hcr.u.dwords[0] ) ) ;
	      i++ ) {
		writel ( hcr.u.dwords[i],
			 arbel->config + ARBEL_HCR_REG ( i ) );
		barrier();
	}

	/* Wait for command completion */
	if ( ( rc = arbel_cmd_wait ( arbel, &hcr ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p timed out waiting for command:\n",
		       arbel );
		DBGC_HD ( arbel, &hcr, sizeof ( hcr ) );
		return rc;
	}

	/* Check command status */
	status = MLX_GET ( &hcr, status );
	if ( status != 0 ) {
		DBGC ( arbel, "Arbel %p command failed with status %02x:\n",
		       arbel, status );
		DBGC_HD ( arbel, &hcr, sizeof ( hcr ) );
		return -EIO;
	}

	/* Read output parameters, if any */
	hcr.u.dwords[3] = readl ( arbel->config + ARBEL_HCR_REG ( 3 ) );
	hcr.u.dwords[4] = readl ( arbel->config + ARBEL_HCR_REG ( 4 ) );
	memcpy ( out, out_buffer, out_len );
	if ( out_len ) {
		DBGC2 ( arbel, "Output:\n" );
		DBGC2_HD ( arbel, out, ( ( out_len < 256 ) ? out_len : 256 ) );
	}

	return 0;
}

static inline int
arbel_cmd_query_dev_lim ( struct arbel *arbel,
			  struct arbelprm_query_dev_lim *dev_lim ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_OUT_CMD ( ARBEL_HCR_QUERY_DEV_LIM, 
					       1, sizeof ( *dev_lim ) ),
			   0, NULL, 0, dev_lim );
}

static inline int
arbel_cmd_sw2hw_cq ( struct arbel *arbel, unsigned long cqn,
		     const struct arbelprm_completion_queue_context *cqctx ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_IN_CMD ( ARBEL_HCR_SW2HW_CQ,
					      1, sizeof ( *cqctx ) ),
			   0, cqctx, cqn, NULL );
}

static inline int
arbel_cmd_hw2sw_cq ( struct arbel *arbel, unsigned long cqn ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_VOID_CMD ( ARBEL_HCR_HW2SW_CQ ),
			   1, NULL, cqn, NULL );
}

static inline int
arbel_cmd_rst2init_qpee ( struct arbel *arbel, unsigned long qpn,
			  const struct arbelprm_qp_ee_state_transitions *ctx ){
	return arbel_cmd ( arbel,
			   ARBEL_HCR_IN_CMD ( ARBEL_HCR_RST2INIT_QPEE,
					      1, sizeof ( *ctx ) ),
			   0, ctx, qpn, NULL );
}

static inline int
arbel_cmd_init2rtr_qpee ( struct arbel *arbel, unsigned long qpn,
			  const struct arbelprm_qp_ee_state_transitions *ctx ){
	return arbel_cmd ( arbel,
			   ARBEL_HCR_IN_CMD ( ARBEL_HCR_INIT2RTR_QPEE,
					      1, sizeof ( *ctx ) ),
			   0, ctx, qpn, NULL );
}

static inline int
arbel_cmd_rtr2rts_qpee ( struct arbel *arbel, unsigned long qpn,
			 const struct arbelprm_qp_ee_state_transitions *ctx ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_IN_CMD ( ARBEL_HCR_RTR2RTS_QPEE,
					      1, sizeof ( *ctx ) ),
			   0, ctx, qpn, NULL );
}

static inline int
arbel_cmd_2rst_qpee ( struct arbel *arbel, unsigned long qpn ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_VOID_CMD ( ARBEL_HCR_2RST_QPEE ),
			   0x03, NULL, qpn, NULL );
}

static inline int
arbel_cmd_mad_ifc ( struct arbel *arbel, union arbelprm_mad *mad ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_INOUT_CMD ( ARBEL_HCR_MAD_IFC,
						 1, sizeof ( *mad ),
						 1, sizeof ( *mad ) ),
			   0x03, mad, PXE_IB_PORT, mad );
}

static inline int
arbel_cmd_read_mgm ( struct arbel *arbel, unsigned int index,
		     struct arbelprm_mgm_entry *mgm ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_OUT_CMD ( ARBEL_HCR_READ_MGM,
					       1, sizeof ( *mgm ) ),
			   0, NULL, index, mgm );
}

static inline int
arbel_cmd_write_mgm ( struct arbel *arbel, unsigned int index,
		      const struct arbelprm_mgm_entry *mgm ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_IN_CMD ( ARBEL_HCR_WRITE_MGM,
					      1, sizeof ( *mgm ) ),
			   0, mgm, index, NULL );
}

static inline int
arbel_cmd_mgid_hash ( struct arbel *arbel, const struct ib_gid *gid,
		      struct arbelprm_mgm_hash *hash ) {
	return arbel_cmd ( arbel,
			   ARBEL_HCR_INOUT_CMD ( ARBEL_HCR_MGID_HASH,
						 1, sizeof ( *gid ),
						 0, sizeof ( *hash ) ),
			   0, gid, 0, hash );
}

/***************************************************************************
 *
 * Completion queue operations
 *
 ***************************************************************************
 */

/**
 * Create completion queue
 *
 * @v ibdev		Infiniband device
 * @v cq		Completion queue
 * @ret rc		Return status code
 */
static int arbel_create_cq ( struct ib_device *ibdev,
			     struct ib_completion_queue *cq ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbel_completion_queue *arbel_cq;
	struct arbelprm_completion_queue_context cqctx;
	struct arbelprm_cq_ci_db_record *ci_db_rec;
	struct arbelprm_cq_arm_db_record *arm_db_rec;
	int cqn_offset;
	unsigned int i;
	int rc;

	/* Find a free completion queue number */
	cqn_offset = arbel_alloc_qn_offset ( arbel->cq_inuse, ARBEL_MAX_CQS );
	if ( cqn_offset < 0 ) {
		DBGC ( arbel, "Arbel %p out of completion queues\n", arbel );
		rc = cqn_offset;
		goto err_cqn_offset;
	}
	cq->cqn = ( arbel->limits.reserved_cqs + cqn_offset );

	/* Allocate control structures */
	arbel_cq = zalloc ( sizeof ( *arbel_cq ) );
	if ( ! arbel_cq ) {
		rc = -ENOMEM;
		goto err_arbel_cq;
	}
	arbel_cq->ci_doorbell_idx = arbel_cq_ci_doorbell_idx ( cqn_offset );
	arbel_cq->arm_doorbell_idx = arbel_cq_arm_doorbell_idx ( cqn_offset );

	/* Allocate completion queue itself */
	arbel_cq->cqe_size = ( cq->num_cqes * sizeof ( arbel_cq->cqe[0] ) );
	arbel_cq->cqe = malloc_dma ( arbel_cq->cqe_size,
				     sizeof ( arbel_cq->cqe[0] ) );
	if ( ! arbel_cq->cqe ) {
		rc = -ENOMEM;
		goto err_cqe;
	}
	memset ( arbel_cq->cqe, 0, arbel_cq->cqe_size );
	for ( i = 0 ; i < cq->num_cqes ; i++ ) {
		MLX_FILL_1 ( &arbel_cq->cqe[i].normal, 7, owner, 1 );
	}
	barrier();

	/* Initialise doorbell records */
	ci_db_rec = &arbel->db_rec[arbel_cq->ci_doorbell_idx].cq_ci;
	MLX_FILL_1 ( ci_db_rec, 0, counter, 0 );
	MLX_FILL_2 ( ci_db_rec, 1,
		     res, ARBEL_UAR_RES_CQ_CI,
		     cq_number, cq->cqn );
	arm_db_rec = &arbel->db_rec[arbel_cq->arm_doorbell_idx].cq_arm;
	MLX_FILL_1 ( arm_db_rec, 0, counter, 0 );
	MLX_FILL_2 ( arm_db_rec, 1,
		     res, ARBEL_UAR_RES_CQ_ARM,
		     cq_number, cq->cqn );

	/* Hand queue over to hardware */
	memset ( &cqctx, 0, sizeof ( cqctx ) );
	MLX_FILL_1 ( &cqctx, 0, st, 0xa /* "Event fired" */ );
	MLX_FILL_1 ( &cqctx, 2, start_address_l,
		     virt_to_bus ( arbel_cq->cqe ) );
	MLX_FILL_2 ( &cqctx, 3,
		     usr_page, arbel->limits.reserved_uars,
		     log_cq_size, fls ( cq->num_cqes - 1 ) );
	MLX_FILL_1 ( &cqctx, 5, c_eqn, arbel->eqn );
	MLX_FILL_1 ( &cqctx, 6, pd, ARBEL_GLOBAL_PD );
	MLX_FILL_1 ( &cqctx, 7, l_key, arbel->reserved_lkey );
	MLX_FILL_1 ( &cqctx, 12, cqn, cq->cqn );
	MLX_FILL_1 ( &cqctx, 13,
		     cq_ci_db_record, arbel_cq->ci_doorbell_idx );
	MLX_FILL_1 ( &cqctx, 14,
		     cq_state_db_record, arbel_cq->arm_doorbell_idx );
	if ( ( rc = arbel_cmd_sw2hw_cq ( arbel, cq->cqn, &cqctx ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p SW2HW_CQ failed: %s\n",
		       arbel, strerror ( rc ) );
		goto err_sw2hw_cq;
	}

	cq->dev_priv = arbel_cq;
	return 0;

 err_sw2hw_cq:
	MLX_FILL_1 ( ci_db_rec, 1, res, ARBEL_UAR_RES_NONE );
	MLX_FILL_1 ( arm_db_rec, 1, res, ARBEL_UAR_RES_NONE );
	free_dma ( arbel_cq->cqe, arbel_cq->cqe_size );
 err_cqe:
	free ( arbel_cq );
 err_arbel_cq:
	arbel_free_qn_offset ( arbel->cq_inuse, cqn_offset );
 err_cqn_offset:
	return rc;
}

/**
 * Destroy completion queue
 *
 * @v ibdev		Infiniband device
 * @v cq		Completion queue
 */
static void arbel_destroy_cq ( struct ib_device *ibdev,
			       struct ib_completion_queue *cq ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbel_completion_queue *arbel_cq = cq->dev_priv;
	struct arbelprm_cq_ci_db_record *ci_db_rec;
	struct arbelprm_cq_arm_db_record *arm_db_rec;
	int cqn_offset;
	int rc;

	/* Take ownership back from hardware */
	if ( ( rc = arbel_cmd_hw2sw_cq ( arbel, cq->cqn ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p FATAL HW2SW_CQ failed on CQN %#lx: "
		       "%s\n", arbel, cq->cqn, strerror ( rc ) );
		/* Leak memory and return; at least we avoid corruption */
		return;
	}

	/* Clear doorbell records */
	ci_db_rec = &arbel->db_rec[arbel_cq->ci_doorbell_idx].cq_ci;
	arm_db_rec = &arbel->db_rec[arbel_cq->arm_doorbell_idx].cq_arm;
	MLX_FILL_1 ( ci_db_rec, 1, res, ARBEL_UAR_RES_NONE );
	MLX_FILL_1 ( arm_db_rec, 1, res, ARBEL_UAR_RES_NONE );

	/* Free memory */
	free_dma ( arbel_cq->cqe, arbel_cq->cqe_size );
	free ( arbel_cq );

	/* Mark queue number as free */
	cqn_offset = ( cq->cqn - arbel->limits.reserved_cqs );
	arbel_free_qn_offset ( arbel->cq_inuse, cqn_offset );

	cq->dev_priv = NULL;
}

/***************************************************************************
 *
 * Queue pair operations
 *
 ***************************************************************************
 */

/**
 * Create send work queue
 *
 * @v arbel_send_wq	Send work queue
 * @v num_wqes		Number of work queue entries
 * @ret rc		Return status code
 */
static int arbel_create_send_wq ( struct arbel_send_work_queue *arbel_send_wq,
				  unsigned int num_wqes ) {
	struct arbelprm_ud_send_wqe *wqe;
	struct arbelprm_ud_send_wqe *next_wqe;
	unsigned int wqe_idx_mask;
	unsigned int i;

	/* Allocate work queue */
	arbel_send_wq->wqe_size = ( num_wqes *
				    sizeof ( arbel_send_wq->wqe[0] ) );
	arbel_send_wq->wqe = malloc_dma ( arbel_send_wq->wqe_size,
					  sizeof ( arbel_send_wq->wqe[0] ) );
	if ( ! arbel_send_wq->wqe )
		return -ENOMEM;
	memset ( arbel_send_wq->wqe, 0, arbel_send_wq->wqe_size );

	/* Link work queue entries */
	wqe_idx_mask = ( num_wqes - 1 );
	for ( i = 0 ; i < num_wqes ; i++ ) {
		wqe = &arbel_send_wq->wqe[i].ud;
		next_wqe = &arbel_send_wq->wqe[ ( i + 1 ) & wqe_idx_mask ].ud;
		MLX_FILL_1 ( &wqe->next, 0, nda_31_6,
			     ( virt_to_bus ( next_wqe ) >> 6 ) );
	}
	
	return 0;
}

/**
 * Create receive work queue
 *
 * @v arbel_recv_wq	Receive work queue
 * @v num_wqes		Number of work queue entries
 * @ret rc		Return status code
 */
static int arbel_create_recv_wq ( struct arbel_recv_work_queue *arbel_recv_wq,
				  unsigned int num_wqes ) {
	struct arbelprm_recv_wqe *wqe;
	struct arbelprm_recv_wqe *next_wqe;
	unsigned int wqe_idx_mask;
	size_t nds;
	unsigned int i;
	unsigned int j;

	/* Allocate work queue */
	arbel_recv_wq->wqe_size = ( num_wqes *
				    sizeof ( arbel_recv_wq->wqe[0] ) );
	arbel_recv_wq->wqe = malloc_dma ( arbel_recv_wq->wqe_size,
					  sizeof ( arbel_recv_wq->wqe[0] ) );
	if ( ! arbel_recv_wq->wqe )
		return -ENOMEM;
	memset ( arbel_recv_wq->wqe, 0, arbel_recv_wq->wqe_size );

	/* Link work queue entries */
	wqe_idx_mask = ( num_wqes - 1 );
	nds = ( ( offsetof ( typeof ( *wqe ), data ) +
		  sizeof ( wqe->data[0] ) ) >> 4 );
	for ( i = 0 ; i < num_wqes ; i++ ) {
		wqe = &arbel_recv_wq->wqe[i].recv;
		next_wqe = &arbel_recv_wq->wqe[( i + 1 ) & wqe_idx_mask].recv;
		MLX_FILL_1 ( &wqe->next, 0, nda_31_6,
			     ( virt_to_bus ( next_wqe ) >> 6 ) );
		MLX_FILL_1 ( &wqe->next, 1, nds, ( sizeof ( *wqe ) / 16 ) );
		for ( j = 0 ; ( ( ( void * ) &wqe->data[j] ) <
				( ( void * ) ( wqe + 1 ) ) ) ; j++ ) {
			MLX_FILL_1 ( &wqe->data[j], 1,
				     l_key, ARBEL_INVALID_LKEY );
		}
	}
	
	return 0;
}

/**
 * Create queue pair
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @ret rc		Return status code
 */
static int arbel_create_qp ( struct ib_device *ibdev,
			     struct ib_queue_pair *qp ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbel_queue_pair *arbel_qp;
	struct arbelprm_qp_ee_state_transitions qpctx;
	struct arbelprm_qp_db_record *send_db_rec;
	struct arbelprm_qp_db_record *recv_db_rec;
	int qpn_offset;
	int rc;

	/* Find a free queue pair number */
	qpn_offset = arbel_alloc_qn_offset ( arbel->qp_inuse, ARBEL_MAX_QPS );
	if ( qpn_offset < 0 ) {
		DBGC ( arbel, "Arbel %p out of queue pairs\n", arbel );
		rc = qpn_offset;
		goto err_qpn_offset;
	}
	qp->qpn = ( ARBEL_QPN_BASE + arbel->limits.reserved_qps + qpn_offset );

	/* Allocate control structures */
	arbel_qp = zalloc ( sizeof ( *arbel_qp ) );
	if ( ! arbel_qp ) {
		rc = -ENOMEM;
		goto err_arbel_qp;
	}
	arbel_qp->send.doorbell_idx = arbel_send_doorbell_idx ( qpn_offset );
	arbel_qp->recv.doorbell_idx = arbel_recv_doorbell_idx ( qpn_offset );

	/* Create send and receive work queues */
	if ( ( rc = arbel_create_send_wq ( &arbel_qp->send,
					   qp->send.num_wqes ) ) != 0 )
		goto err_create_send_wq;
	if ( ( rc = arbel_create_recv_wq ( &arbel_qp->recv,
					   qp->recv.num_wqes ) ) != 0 )
		goto err_create_recv_wq;

	/* Initialise doorbell records */
	send_db_rec = &arbel->db_rec[arbel_qp->send.doorbell_idx].qp;
	MLX_FILL_1 ( send_db_rec, 0, counter, 0 );
	MLX_FILL_2 ( send_db_rec, 1,
		     res, ARBEL_UAR_RES_SQ,
		     qp_number, qp->qpn );
	recv_db_rec = &arbel->db_rec[arbel_qp->recv.doorbell_idx].qp;
	MLX_FILL_1 ( recv_db_rec, 0, counter, 0 );
	MLX_FILL_2 ( recv_db_rec, 1,
		     res, ARBEL_UAR_RES_RQ,
		     qp_number, qp->qpn );

	/* Hand queue over to hardware */
	memset ( &qpctx, 0, sizeof ( qpctx ) );
	MLX_FILL_3 ( &qpctx, 2,
		     qpc_eec_data.de, 1,
		     qpc_eec_data.pm_state, 0x03 /* Always 0x03 for UD */,
		     qpc_eec_data.st, ARBEL_ST_UD );
	MLX_FILL_6 ( &qpctx, 4,
		     qpc_eec_data.mtu, ARBEL_MTU_2048,
		     qpc_eec_data.msg_max, 11 /* 2^11 = 2048 */,
		     qpc_eec_data.log_rq_size, fls ( qp->recv.num_wqes - 1 ),
		     qpc_eec_data.log_rq_stride,
		     ( fls ( sizeof ( arbel_qp->recv.wqe[0] ) - 1 ) - 4 ),
		     qpc_eec_data.log_sq_size, fls ( qp->send.num_wqes - 1 ),
		     qpc_eec_data.log_sq_stride,
		     ( fls ( sizeof ( arbel_qp->send.wqe[0] ) - 1 ) - 4 ) );
	MLX_FILL_1 ( &qpctx, 5,
		     qpc_eec_data.usr_page, arbel->limits.reserved_uars );
	MLX_FILL_1 ( &qpctx, 10, qpc_eec_data.primary_address_path.port_number,
		     PXE_IB_PORT );
	MLX_FILL_1 ( &qpctx, 27, qpc_eec_data.pd, ARBEL_GLOBAL_PD );
	MLX_FILL_1 ( &qpctx, 29, qpc_eec_data.wqe_lkey, arbel->reserved_lkey );
	MLX_FILL_1 ( &qpctx, 30, qpc_eec_data.ssc, 1 );
	MLX_FILL_1 ( &qpctx, 33, qpc_eec_data.cqn_snd, qp->send.cq->cqn );
	MLX_FILL_1 ( &qpctx, 34, qpc_eec_data.snd_wqe_base_adr_l,
		     ( virt_to_bus ( arbel_qp->send.wqe ) >> 6 ) );
	MLX_FILL_1 ( &qpctx, 35, qpc_eec_data.snd_db_record_index,
		     arbel_qp->send.doorbell_idx );
	MLX_FILL_1 ( &qpctx, 38, qpc_eec_data.rsc, 1 );
	MLX_FILL_1 ( &qpctx, 41, qpc_eec_data.cqn_rcv, qp->recv.cq->cqn );
	MLX_FILL_1 ( &qpctx, 42, qpc_eec_data.rcv_wqe_base_adr_l,
		     ( virt_to_bus ( arbel_qp->recv.wqe ) >> 6 ) );
	MLX_FILL_1 ( &qpctx, 43, qpc_eec_data.rcv_db_record_index,
		     arbel_qp->recv.doorbell_idx );
	MLX_FILL_1 ( &qpctx, 44, qpc_eec_data.q_key, qp->qkey );
	if ( ( rc = arbel_cmd_rst2init_qpee ( arbel, qp->qpn, &qpctx )) != 0 ){
		DBGC ( arbel, "Arbel %p RST2INIT_QPEE failed: %s\n",
		       arbel, strerror ( rc ) );
		goto err_rst2init_qpee;
	}
	memset ( &qpctx, 0, sizeof ( qpctx ) );
	MLX_FILL_2 ( &qpctx, 4,
		     qpc_eec_data.mtu, ARBEL_MTU_2048,
		     qpc_eec_data.msg_max, 11 /* 2^11 = 2048 */ );
	if ( ( rc = arbel_cmd_init2rtr_qpee ( arbel, qp->qpn, &qpctx )) != 0 ){
		DBGC ( arbel, "Arbel %p INIT2RTR_QPEE failed: %s\n",
		       arbel, strerror ( rc ) );
		goto err_init2rtr_qpee;
	}
	memset ( &qpctx, 0, sizeof ( qpctx ) );
	if ( ( rc = arbel_cmd_rtr2rts_qpee ( arbel, qp->qpn, &qpctx ) ) != 0 ){
		DBGC ( arbel, "Arbel %p RTR2RTS_QPEE failed: %s\n",
		       arbel, strerror ( rc ) );
		goto err_rtr2rts_qpee;
	}

	qp->dev_priv = arbel_qp;
	return 0;

 err_rtr2rts_qpee:
 err_init2rtr_qpee:
	arbel_cmd_2rst_qpee ( arbel, qp->qpn );
 err_rst2init_qpee:
	MLX_FILL_1 ( send_db_rec, 1, res, ARBEL_UAR_RES_NONE );
	MLX_FILL_1 ( recv_db_rec, 1, res, ARBEL_UAR_RES_NONE );
	free_dma ( arbel_qp->recv.wqe, arbel_qp->recv.wqe_size );
 err_create_recv_wq:
	free_dma ( arbel_qp->send.wqe, arbel_qp->send.wqe_size );
 err_create_send_wq:
	free ( arbel_qp );
 err_arbel_qp:
	arbel_free_qn_offset ( arbel->qp_inuse, qpn_offset );
 err_qpn_offset:
	return rc;
}

/**
 * Destroy queue pair
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 */
static void arbel_destroy_qp ( struct ib_device *ibdev,
			       struct ib_queue_pair *qp ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbel_queue_pair *arbel_qp = qp->dev_priv;
	struct arbelprm_qp_db_record *send_db_rec;
	struct arbelprm_qp_db_record *recv_db_rec;
	int qpn_offset;
	int rc;

	/* Take ownership back from hardware */
	if ( ( rc = arbel_cmd_2rst_qpee ( arbel, qp->qpn ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p FATAL 2RST_QPEE failed on QPN %#lx: "
		       "%s\n", arbel, qp->qpn, strerror ( rc ) );
		/* Leak memory and return; at least we avoid corruption */
		return;
	}

	/* Clear doorbell records */
	send_db_rec = &arbel->db_rec[arbel_qp->send.doorbell_idx].qp;
	recv_db_rec = &arbel->db_rec[arbel_qp->recv.doorbell_idx].qp;
	MLX_FILL_1 ( send_db_rec, 1, res, ARBEL_UAR_RES_NONE );
	MLX_FILL_1 ( recv_db_rec, 1, res, ARBEL_UAR_RES_NONE );

	/* Free memory */
	free_dma ( arbel_qp->send.wqe, arbel_qp->send.wqe_size );
	free_dma ( arbel_qp->recv.wqe, arbel_qp->recv.wqe_size );
	free ( arbel_qp );

	/* Mark queue number as free */
	qpn_offset = ( qp->qpn - ARBEL_QPN_BASE - arbel->limits.reserved_qps );
	arbel_free_qn_offset ( arbel->qp_inuse, qpn_offset );

	qp->dev_priv = NULL;
}

/***************************************************************************
 *
 * Work request operations
 *
 ***************************************************************************
 */

/**
 * Ring doorbell register in UAR
 *
 * @v arbel		Arbel device
 * @v db_reg		Doorbell register structure
 * @v offset		Address of doorbell
 */
static void arbel_ring_doorbell ( struct arbel *arbel,
				  union arbelprm_doorbell_register *db_reg,
				  unsigned int offset ) {

	DBGC2 ( arbel, "Arbel %p ringing doorbell %08lx:%08lx at %lx\n",
		arbel, db_reg->dword[0], db_reg->dword[1],
		virt_to_phys ( arbel->uar + offset ) );

	barrier();
	writel ( db_reg->dword[0], ( arbel->uar + offset + 0 ) );
	barrier();
	writel ( db_reg->dword[1], ( arbel->uar + offset + 4 ) );
}

/** GID used for GID-less send work queue entries */
static const struct ib_gid arbel_no_gid = {
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0 } }
};

/**
 * Post send work queue entry
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @v av		Address vector
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
static int arbel_post_send ( struct ib_device *ibdev,
			     struct ib_queue_pair *qp,
			     struct ib_address_vector *av,
			     struct io_buffer *iobuf ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbel_queue_pair *arbel_qp = qp->dev_priv;
	struct ib_work_queue *wq = &qp->send;
	struct arbel_send_work_queue *arbel_send_wq = &arbel_qp->send;
	struct arbelprm_ud_send_wqe *prev_wqe;
	struct arbelprm_ud_send_wqe *wqe;
	struct arbelprm_qp_db_record *qp_db_rec;
	union arbelprm_doorbell_register db_reg;
	const struct ib_gid *gid;
	unsigned int wqe_idx_mask;
	size_t nds;

	/* Allocate work queue entry */
	wqe_idx_mask = ( wq->num_wqes - 1 );
	if ( wq->iobufs[wq->next_idx & wqe_idx_mask] ) {
		DBGC ( arbel, "Arbel %p send queue full", arbel );
		return -ENOBUFS;
	}
	wq->iobufs[wq->next_idx & wqe_idx_mask] = iobuf;
	prev_wqe = &arbel_send_wq->wqe[(wq->next_idx - 1) & wqe_idx_mask].ud;
	wqe = &arbel_send_wq->wqe[wq->next_idx & wqe_idx_mask].ud;

	/* Construct work queue entry */
	MLX_FILL_1 ( &wqe->next, 1, always1, 1 );
	memset ( &wqe->ctrl, 0, sizeof ( wqe->ctrl ) );
	MLX_FILL_1 ( &wqe->ctrl, 0, always1, 1 );
	memset ( &wqe->ud, 0, sizeof ( wqe->ud ) );
	MLX_FILL_2 ( &wqe->ud, 0,
		     ud_address_vector.pd, ARBEL_GLOBAL_PD,
		     ud_address_vector.port_number, PXE_IB_PORT );
	MLX_FILL_2 ( &wqe->ud, 1,
		     ud_address_vector.rlid, av->dlid,
		     ud_address_vector.g, av->gid_present );
	MLX_FILL_2 ( &wqe->ud, 2,
		     ud_address_vector.max_stat_rate,
			 ( ( av->rate >= 3 ) ? 0 : 1 ),
		     ud_address_vector.msg, 3 );
	MLX_FILL_1 ( &wqe->ud, 3, ud_address_vector.sl, av->sl );
	gid = ( av->gid_present ? &av->gid : &arbel_no_gid );
	memcpy ( &wqe->ud.u.dwords[4], gid, sizeof ( *gid ) );
	MLX_FILL_1 ( &wqe->ud, 8, destination_qp, av->dest_qp );
	MLX_FILL_1 ( &wqe->ud, 9, q_key, av->qkey );
	MLX_FILL_1 ( &wqe->data[0], 0, byte_count, iob_len ( iobuf ) );
	MLX_FILL_1 ( &wqe->data[0], 1, l_key, arbel->reserved_lkey );
	MLX_FILL_1 ( &wqe->data[0], 3,
		     local_address_l, virt_to_bus ( iobuf->data ) );

	/* Update previous work queue entry's "next" field */
	nds = ( ( offsetof ( typeof ( *wqe ), data ) +
		  sizeof ( wqe->data[0] ) ) >> 4 );
	MLX_SET ( &prev_wqe->next, nopcode, ARBEL_OPCODE_SEND );
	MLX_FILL_3 ( &prev_wqe->next, 1,
		     nds, nds,
		     f, 1,
		     always1, 1 );

	/* Update doorbell record */
	barrier();
	qp_db_rec = &arbel->db_rec[arbel_send_wq->doorbell_idx].qp;
	MLX_FILL_1 ( qp_db_rec, 0,
		     counter, ( ( wq->next_idx + 1 ) & 0xffff ) );

	/* Ring doorbell register */
	MLX_FILL_4 ( &db_reg.send, 0,
		     nopcode, ARBEL_OPCODE_SEND,
		     f, 1,
		     wqe_counter, ( wq->next_idx & 0xffff ),
		     wqe_cnt, 1 );
	MLX_FILL_2 ( &db_reg.send, 1,
		     nds, nds,
		     qpn, qp->qpn );
	arbel_ring_doorbell ( arbel, &db_reg, POST_SND_OFFSET );

	/* Update work queue's index */
	wq->next_idx++;

	return 0;
}

/**
 * Post receive work queue entry
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
static int arbel_post_recv ( struct ib_device *ibdev,
			     struct ib_queue_pair *qp,
			     struct io_buffer *iobuf ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbel_queue_pair *arbel_qp = qp->dev_priv;
	struct ib_work_queue *wq = &qp->recv;
	struct arbel_recv_work_queue *arbel_recv_wq = &arbel_qp->recv;
	struct arbelprm_recv_wqe *wqe;
	union arbelprm_doorbell_record *db_rec;
	unsigned int wqe_idx_mask;

	/* Allocate work queue entry */
	wqe_idx_mask = ( wq->num_wqes - 1 );
	if ( wq->iobufs[wq->next_idx & wqe_idx_mask] ) {
		DBGC ( arbel, "Arbel %p receive queue full", arbel );
		return -ENOBUFS;
	}
	wq->iobufs[wq->next_idx & wqe_idx_mask] = iobuf;
	wqe = &arbel_recv_wq->wqe[wq->next_idx & wqe_idx_mask].recv;

	/* Construct work queue entry */
	MLX_FILL_1 ( &wqe->data[0], 0, byte_count, iob_tailroom ( iobuf ) );
	MLX_FILL_1 ( &wqe->data[0], 1, l_key, arbel->reserved_lkey );
	MLX_FILL_1 ( &wqe->data[0], 3,
		     local_address_l, virt_to_bus ( iobuf->data ) );

	/* Update doorbell record */
	barrier();
	db_rec = &arbel->db_rec[arbel_recv_wq->doorbell_idx];
	MLX_FILL_1 ( &db_rec->qp, 0,
		     counter, ( ( wq->next_idx + 1 ) & 0xffff ) );	

	/* Update work queue's index */
	wq->next_idx++;

	return 0;	
}

/**
 * Handle completion
 *
 * @v ibdev		Infiniband device
 * @v cq		Completion queue
 * @v cqe		Hardware completion queue entry
 * @v complete_send	Send completion handler
 * @v complete_recv	Receive completion handler
 * @ret rc		Return status code
 */
static int arbel_complete ( struct ib_device *ibdev,
			    struct ib_completion_queue *cq,
			    union arbelprm_completion_entry *cqe,
			    ib_completer_t complete_send,
			    ib_completer_t complete_recv ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct ib_completion completion;
	struct ib_work_queue *wq;
	struct ib_queue_pair *qp;
	struct arbel_queue_pair *arbel_qp;
	struct arbel_send_work_queue *arbel_send_wq;
	struct arbel_recv_work_queue *arbel_recv_wq;
	struct io_buffer *iobuf;
	ib_completer_t complete;
	unsigned int opcode;
	unsigned long qpn;
	int is_send;
	unsigned long wqe_adr;
	unsigned int wqe_idx;
	int rc = 0;

	/* Parse completion */
	memset ( &completion, 0, sizeof ( completion ) );
	completion.len = MLX_GET ( &cqe->normal, byte_cnt );
	qpn = MLX_GET ( &cqe->normal, my_qpn );
	is_send = MLX_GET ( &cqe->normal, s );
	wqe_adr = ( MLX_GET ( &cqe->normal, wqe_adr ) << 6 );
	opcode = MLX_GET ( &cqe->normal, opcode );
	if ( opcode >= ARBEL_OPCODE_RECV_ERROR ) {
		/* "s" field is not valid for error opcodes */
		is_send = ( opcode == ARBEL_OPCODE_SEND_ERROR );
		completion.syndrome = MLX_GET ( &cqe->error, syndrome );
		DBGC ( arbel, "Arbel %p CPN %lx syndrome %x vendor %lx\n",
		       arbel, cq->cqn, completion.syndrome,
		       MLX_GET ( &cqe->error, vendor_code ) );
		rc = -EIO;
		/* Don't return immediately; propagate error to completer */
	}

	/* Identify work queue */
	wq = ib_find_wq ( cq, qpn, is_send );
	if ( ! wq ) {
		DBGC ( arbel, "Arbel %p CQN %lx unknown %s QPN %lx\n",
		       arbel, cq->cqn, ( is_send ? "send" : "recv" ), qpn );
		return -EIO;
	}
	qp = wq->qp;
	arbel_qp = qp->dev_priv;

	/* Identify work queue entry index */
	if ( is_send ) {
		arbel_send_wq = &arbel_qp->send;
		wqe_idx = ( ( wqe_adr - virt_to_bus ( arbel_send_wq->wqe ) ) /
			    sizeof ( arbel_send_wq->wqe[0] ) );
	} else {
		arbel_recv_wq = &arbel_qp->recv;
		wqe_idx = ( ( wqe_adr - virt_to_bus ( arbel_recv_wq->wqe ) ) /
			    sizeof ( arbel_recv_wq->wqe[0] ) );
	}

	/* Identify I/O buffer */
	iobuf = wq->iobufs[wqe_idx];
	if ( ! iobuf ) {
		DBGC ( arbel, "Arbel %p CQN %lx QPN %lx empty WQE %x\n",
		       arbel, cq->cqn, qpn, wqe_idx );
		return -EIO;
	}
	wq->iobufs[wqe_idx] = NULL;

	/* Pass off to caller's completion handler */
	complete = ( is_send ? complete_send : complete_recv );
	complete ( ibdev, qp, &completion, iobuf );

	return rc;
}			     

/**
 * Drain event queue
 *
 * @v arbel		Arbel device
 */
static void arbel_drain_eq ( struct arbel *arbel ) {
#warning "drain the event queue"
}

/**
 * Poll completion queue
 *
 * @v ibdev		Infiniband device
 * @v cq		Completion queue
 * @v complete_send	Send completion handler
 * @v complete_recv	Receive completion handler
 */
static void arbel_poll_cq ( struct ib_device *ibdev,
			    struct ib_completion_queue *cq,
			    ib_completer_t complete_send,
			    ib_completer_t complete_recv ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbel_completion_queue *arbel_cq = cq->dev_priv;
	struct arbelprm_cq_ci_db_record *ci_db_rec;
	union arbelprm_completion_entry *cqe;
	unsigned int cqe_idx_mask;
	int rc;

	/* Drain the event queue */
	arbel_drain_eq ( arbel );

	while ( 1 ) {
		/* Look for completion entry */
		cqe_idx_mask = ( cq->num_cqes - 1 );
		cqe = &arbel_cq->cqe[cq->next_idx & cqe_idx_mask];
		if ( MLX_GET ( &cqe->normal, owner ) != 0 ) {
			/* Entry still owned by hardware; end of poll */
			break;
		}

		/* Handle completion */
		if ( ( rc = arbel_complete ( ibdev, cq, cqe, complete_send,
					     complete_recv ) ) != 0 ) {
			DBGC ( arbel, "Arbel %p failed to complete: %s\n",
			       arbel, strerror ( rc ) );
			DBGC_HD ( arbel, cqe, sizeof ( *cqe ) );
		}

		/* Return ownership to hardware */
		MLX_FILL_1 ( &cqe->normal, 7, owner, 1 );
		barrier();
		/* Update completion queue's index */
		cq->next_idx++;
		/* Update doorbell record */
		ci_db_rec = &arbel->db_rec[arbel_cq->ci_doorbell_idx].cq_ci;
		MLX_FILL_1 ( ci_db_rec, 0,
			     counter, ( cq->next_idx & 0xffffffffUL ) );
	}
}

/***************************************************************************
 *
 * Multicast group operations
 *
 ***************************************************************************
 */

/**
 * Attach to multicast group
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @v gid		Multicast GID
 * @ret rc		Return status code
 */
static int arbel_mcast_attach ( struct ib_device *ibdev,
				struct ib_queue_pair *qp,
				struct ib_gid *gid ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbelprm_mgm_hash hash;
	struct arbelprm_mgm_entry mgm;
	unsigned int index;
	int rc;

	/* Generate hash table index */
	if ( ( rc = arbel_cmd_mgid_hash ( arbel, gid, &hash ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not hash GID: %s\n",
		       arbel, strerror ( rc ) );
		return rc;
	}
	index = MLX_GET ( &hash, hash );

	/* Check for existing hash table entry */
	if ( ( rc = arbel_cmd_read_mgm ( arbel, index, &mgm ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not read MGM %#x: %s\n",
		       arbel, index, strerror ( rc ) );
		return rc;
	}
	if ( MLX_GET ( &mgm, mgmqp_0.qi ) != 0 ) {
		/* FIXME: this implementation allows only a single QP
		 * per multicast group, and doesn't handle hash
		 * collisions.  Sufficient for IPoIB but may need to
		 * be extended in future.
		 */
		DBGC ( arbel, "Arbel %p MGID index %#x already in use\n",
		       arbel, index );
		return -EBUSY;
	}

	/* Update hash table entry */
	MLX_FILL_2 ( &mgm, 8,
		     mgmqp_0.qpn_i, qp->qpn,
		     mgmqp_0.qi, 1 );
	memcpy ( &mgm.u.dwords[4], gid, sizeof ( *gid ) );
	if ( ( rc = arbel_cmd_write_mgm ( arbel, index, &mgm ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not write MGM %#x: %s\n",
		       arbel, index, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Detach from multicast group
 *
 * @v ibdev		Infiniband device
 * @v qp		Queue pair
 * @v gid		Multicast GID
 */
static void arbel_mcast_detach ( struct ib_device *ibdev,
				 struct ib_queue_pair *qp __unused,
				 struct ib_gid *gid ) {
	struct arbel *arbel = ibdev->dev_priv;
	struct arbelprm_mgm_hash hash;
	struct arbelprm_mgm_entry mgm;
	unsigned int index;
	int rc;

	/* Generate hash table index */
	if ( ( rc = arbel_cmd_mgid_hash ( arbel, gid, &hash ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not hash GID: %s\n",
		       arbel, strerror ( rc ) );
		return;
	}
	index = MLX_GET ( &hash, hash );

	/* Clear hash table entry */
	memset ( &mgm, 0, sizeof ( mgm ) );
	if ( ( rc = arbel_cmd_write_mgm ( arbel, index, &mgm ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not write MGM %#x: %s\n",
		       arbel, index, strerror ( rc ) );
		return;
	}
}

/** Arbel Infiniband operations */
static struct ib_device_operations arbel_ib_operations = {
	.create_cq	= arbel_create_cq,
	.destroy_cq	= arbel_destroy_cq,
	.create_qp	= arbel_create_qp,
	.destroy_qp	= arbel_destroy_qp,
	.post_send	= arbel_post_send,
	.post_recv	= arbel_post_recv,
	.poll_cq	= arbel_poll_cq,
	.mcast_attach	= arbel_mcast_attach,
	.mcast_detach	= arbel_mcast_detach,
};


static int arbel_mad_ifc ( struct arbel *arbel,
			   union arbelprm_mad *mad ) {
	struct ib_mad_hdr *hdr = &mad->mad.mad_hdr;
	int rc;

	hdr->base_version = IB_MGMT_BASE_VERSION;
	if ( ( rc = arbel_cmd_mad_ifc ( arbel, mad ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not issue MAD IFC: %s\n",
		       arbel, strerror ( rc ) );
		return rc;
	}
	if ( hdr->status != 0 ) {
		DBGC ( arbel, "Arbel %p MAD IFC status %04x\n",
		       arbel, ntohs ( hdr->status ) );
		return -EIO;
	}
	return 0;
}

static int arbel_get_port_info ( struct arbel *arbel,
				 struct ib_mad_port_info *port_info ) {
	union arbelprm_mad mad;
	struct ib_mad_hdr *hdr = &mad.mad.mad_hdr;
	int rc;

	memset ( &mad, 0, sizeof ( mad ) );
	hdr->mgmt_class = IB_MGMT_CLASS_SUBN_LID_ROUTED;
	hdr->class_version = 1;
	hdr->method = IB_MGMT_METHOD_GET;
	hdr->attr_id = htons ( IB_SMP_ATTR_PORT_INFO );
	hdr->attr_mod = htonl ( PXE_IB_PORT );
	if ( ( rc = arbel_mad_ifc ( arbel, &mad ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not get port info: %s\n",
		       arbel, strerror ( rc ) );
		return rc;
	}
	memcpy ( port_info, &mad.mad.port_info, sizeof ( *port_info ) );
	return 0;
}

static int arbel_get_guid_info ( struct arbel *arbel,
				 struct ib_mad_guid_info *guid_info ) {
	union arbelprm_mad mad;
	struct ib_mad_hdr *hdr = &mad.mad.mad_hdr;
	int rc;

	memset ( &mad, 0, sizeof ( mad ) );
	hdr->mgmt_class = IB_MGMT_CLASS_SUBN_LID_ROUTED;
	hdr->class_version = 1;
	hdr->method = IB_MGMT_METHOD_GET;
	hdr->attr_id = htons ( IB_SMP_ATTR_GUID_INFO );
	if ( ( rc = arbel_mad_ifc ( arbel, &mad ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not get GUID info: %s\n",
		       arbel, strerror ( rc ) );
		return rc;
	}
	memcpy ( guid_info, &mad.mad.guid_info, sizeof ( *guid_info ) );
	return 0;
}

static int arbel_get_pkey_table ( struct arbel *arbel,
				  struct ib_mad_pkey_table *pkey_table ) {
	union arbelprm_mad mad;
	struct ib_mad_hdr *hdr = &mad.mad.mad_hdr;
	int rc;

	memset ( &mad, 0, sizeof ( mad ) );
	hdr->mgmt_class = IB_MGMT_CLASS_SUBN_LID_ROUTED;
	hdr->class_version = 1;
	hdr->method = IB_MGMT_METHOD_GET;
	hdr->attr_id = htons ( IB_SMP_ATTR_PKEY_TABLE );
	if ( ( rc = arbel_mad_ifc ( arbel, &mad ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not get pkey table: %s\n",
		       arbel, strerror ( rc ) );
		return rc;
	}
	memcpy ( pkey_table, &mad.mad.pkey_table, sizeof ( *pkey_table ) );
	return 0;
}

static int arbel_get_port_gid ( struct arbel *arbel,
				struct ib_gid *port_gid ) {
	union {
		/* This union exists just to save stack space */
		struct ib_mad_port_info port_info;
		struct ib_mad_guid_info guid_info;
	} u;
	int rc;

	/* Port info gives us the first half of the port GID */
	if ( ( rc = arbel_get_port_info ( arbel, &u.port_info ) ) != 0 )
		return rc;
	memcpy ( &port_gid->u.bytes[0], u.port_info.gid_prefix, 8 );
	
	/* GUID info gives us the second half of the port GID */
	if ( ( rc = arbel_get_guid_info ( arbel, &u.guid_info ) ) != 0 )
		return rc;
	memcpy ( &port_gid->u.bytes[8], u.guid_info.gid_local, 8 );

	return 0;
}

static int arbel_get_sm_lid ( struct arbel *arbel,
			      unsigned long *sm_lid ) {
	struct ib_mad_port_info port_info;
	int rc;

	if ( ( rc = arbel_get_port_info ( arbel, &port_info ) ) != 0 )
		return rc;
	*sm_lid = ntohs ( port_info.mastersm_lid );
	return 0;
}

static int arbel_get_broadcast_gid ( struct arbel *arbel,
				     struct ib_gid *broadcast_gid ) {
	static const struct ib_gid ipv4_broadcast_gid = {
		{ { 0xff, 0x12, 0x40, 0x1b, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff } }
	};
	struct ib_mad_pkey_table pkey_table;
	int rc;

	/* Start with the IPv4 broadcast GID */
	memcpy ( broadcast_gid, &ipv4_broadcast_gid,
		 sizeof ( *broadcast_gid ) );

	/* Add partition key */
	if ( ( rc = arbel_get_pkey_table ( arbel, &pkey_table ) ) != 0 )
		return rc;
	memcpy ( &broadcast_gid->u.bytes[4], &pkey_table.pkey[0][0],
		 sizeof ( pkey_table.pkey[0][0] ) );

	return 0;
}

/**
 * Probe PCI device
 *
 * @v pci		PCI device
 * @v id		PCI ID
 * @ret rc		Return status code
 */
static int arbel_probe ( struct pci_device *pci,
			 const struct pci_device_id *id __unused ) {
	struct ib_device *ibdev;
	struct arbelprm_query_dev_lim dev_lim;
	struct arbel *arbel;
	udqp_t qph;
	int rc;

	/* Allocate Infiniband device */
	ibdev = alloc_ibdev ( sizeof ( *arbel ) );
	if ( ! ibdev )
		return -ENOMEM;
	ibdev->op = &arbel_ib_operations;
	pci_set_drvdata ( pci, ibdev );
	ibdev->dev = &pci->dev;
	arbel = ibdev->dev_priv;
	memset ( arbel, 0, sizeof ( *arbel ) );

	/* Fix up PCI device */
	adjust_pci_device ( pci );

	/* Initialise hardware */
	if ( ( rc = ib_driver_init ( pci, &qph ) ) != 0 )
		goto err_ib_driver_init;

	/* Hack up IB structures */
	arbel->config = memfree_pci_dev.cr_space;
	arbel->mailbox_in = dev_buffers_p->inprm_buf;
	arbel->mailbox_out = dev_buffers_p->outprm_buf;
	arbel->uar = memfree_pci_dev.uar;
	arbel->db_rec = dev_ib_data.uar_context_base;
	arbel->reserved_lkey = dev_ib_data.mkey;
	arbel->eqn = dev_ib_data.eq.eqn;

	/* Get device limits */
	if ( ( rc = arbel_cmd_query_dev_lim ( arbel, &dev_lim ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not get device limits: %s\n",
		       arbel, strerror ( rc ) );
		goto err_query_dev_lim;
	}
	arbel->limits.reserved_uars = MLX_GET ( &dev_lim, num_rsvd_uars );
	arbel->limits.reserved_cqs =
		( 1 << MLX_GET ( &dev_lim, log2_rsvd_cqs ) );
	arbel->limits.reserved_qps =
		( 1 << MLX_GET ( &dev_lim, log2_rsvd_qps ) );

	/* Get subnet manager LID */
	if ( ( rc = arbel_get_sm_lid ( arbel, &ibdev->sm_lid ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not determine subnet manager "
		       "LID: %s\n", arbel, strerror ( rc ) );
		goto err_get_sm_lid;
	}

	/* Get port GID */
	if ( ( rc = arbel_get_port_gid ( arbel, &ibdev->port_gid ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not determine port GID: %s\n",
		       arbel, strerror ( rc ) );
		goto err_get_port_gid;
	}

	/* Get broadcast GID */
	if ( ( rc = arbel_get_broadcast_gid ( arbel,
					      &ibdev->broadcast_gid ) ) != 0 ){
		DBGC ( arbel, "Arbel %p could not determine broadcast GID: "
		       "%s\n", arbel, strerror ( rc ) );
		goto err_get_broadcast_gid;
	}

	struct ud_av_st *bcast_av = ib_data.bcast_av;
	struct arbelprm_ud_address_vector *bav =
		( struct arbelprm_ud_address_vector * ) &bcast_av->av;
	struct ib_address_vector *av = &hack_ipoib_bcast_av;
	av->dest_qp = bcast_av->dest_qp;
	av->qkey = bcast_av->qkey;
	av->dlid = MLX_GET ( bav, rlid );
	av->rate = ( MLX_GET ( bav, max_stat_rate ) ? 1 : 4 );
	av->sl = MLX_GET ( bav, sl );
	av->gid_present = 1;
	memcpy ( &av->gid, ( ( void * ) bav ) + 16, 16 );

	/* Add IPoIB device */
	if ( ( rc = ipoib_probe ( ibdev ) ) != 0 ) {
		DBGC ( arbel, "Arbel %p could not add IPoIB device: %s\n",
		       arbel, strerror ( rc ) );
		goto err_ipoib_probe;
	}

	return 0;

 err_ipoib_probe:
 err_get_broadcast_gid:
 err_get_port_gid:
 err_get_sm_lid:
 err_query_dev_lim:
	ib_driver_close ( 0 );
 err_ib_driver_init:
	free_ibdev ( ibdev );
	return rc;
}

/**
 * Remove PCI device
 *
 * @v pci		PCI device
 */
static void arbel_remove ( struct pci_device *pci ) {
	struct ib_device *ibdev = pci_get_drvdata ( pci );

	ipoib_remove ( ibdev );
	ib_driver_close ( 0 );
}

static struct pci_device_id arbel_nics[] = {
	PCI_ROM ( 0x15b3, 0x6282, "MT25218", "MT25218 HCA driver" ),
	PCI_ROM ( 0x15b3, 0x6274, "MT25204", "MT25204 HCA driver" ),
};

struct pci_driver arbel_driver __pci_driver = {
	.ids = arbel_nics,
	.id_count = ( sizeof ( arbel_nics ) / sizeof ( arbel_nics[0] ) ),
	.probe = arbel_probe,
	.remove = arbel_remove,
};
