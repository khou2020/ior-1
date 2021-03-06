/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 */
/*
 * Copyright (C) 2018-2020 Intel Corporation
 * See the file COPYRIGHT for a complete copyright notice and license.
 */

/*
 * This file implements the abstract I/O interface for DAOS Array API.
 */

#define _BSD_SOURCE

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <strings.h>
#include <sys/types.h>
#include <libgen.h>
#include <stdbool.h>

#include <mpi.h>
#include <gurt/common.h>
#include <daos.h>

#include "aiori.h"
#include "utilities.h"
#include "iordef.h"

/************************** O P T I O N S *****************************/
typedef struct {
        char		*pool;
        char		*svcl;
        char		*group;
	char		*cont;
	int		chunk_size;
	int		destroy;
	char		*oclass;
} DAOS_options_t;

static option_help * DAOS_options(aiori_mod_opt_t ** init_backend_options,
                                  aiori_mod_opt_t * init_values){
        DAOS_options_t * o = malloc(sizeof(DAOS_options_t));

        if (init_values != NULL) {
                memcpy(o, init_values, sizeof(DAOS_options_t));
        } else {
                memset(o, 0, sizeof(DAOS_options_t));
                /* initialize the options properly */
                o->chunk_size	= 1048576;
        }

        *init_backend_options = (aiori_mod_opt_t *) o;

        option_help h [] = {
                {0, "daos.pool", "pool uuid", OPTION_OPTIONAL_ARGUMENT, 's', &o->pool},
                {0, "daos.svcl", "pool SVCL", OPTION_OPTIONAL_ARGUMENT, 's', &o->svcl},
                {0, "daos.group", "server group", OPTION_OPTIONAL_ARGUMENT, 's', &o->group},
                {0, "daos.cont", "container uuid", OPTION_OPTIONAL_ARGUMENT, 's', &o->cont},
                {0, "daos.chunk_size", "chunk size", OPTION_OPTIONAL_ARGUMENT, 'd', &o->chunk_size},
                {0, "daos.destroy", "Destroy Container", OPTION_FLAG, 'd', &o->destroy},
                {0, "daos.oclass", "object class", OPTION_OPTIONAL_ARGUMENT, 's', &o->oclass},
                LAST_OPTION
        };

        option_help * help = malloc(sizeof(h));
        memcpy(help, h, sizeof(h));
        return help;
}

/**************************** P R O T O T Y P E S *****************************/

static void DAOS_Init(aiori_mod_opt_t *);
static void DAOS_Fini(aiori_mod_opt_t *);
static aiori_fd_t *DAOS_Create(char *, int, aiori_mod_opt_t *);
static aiori_fd_t *DAOS_Open(char *, int, aiori_mod_opt_t *);
static int DAOS_Access(const char *, int, aiori_mod_opt_t *);
static IOR_offset_t DAOS_Xfer(int, aiori_fd_t *, IOR_size_t *, IOR_offset_t,
                              IOR_offset_t, aiori_mod_opt_t *);
static void DAOS_Close(aiori_fd_t *, aiori_mod_opt_t *);
static void DAOS_Delete(char *, aiori_mod_opt_t *);
static char* DAOS_GetVersion();
static void DAOS_Fsync(aiori_fd_t *, aiori_mod_opt_t *);
static IOR_offset_t DAOS_GetFileSize(aiori_mod_opt_t *, char *);
static option_help * DAOS_options();
static void DAOS_init_xfer_options(aiori_xfer_hint_t *);
static int DAOS_check_params(aiori_mod_opt_t *);

/************************** D E C L A R A T I O N S ***************************/

ior_aiori_t daos_aiori = {
        .name		= "DAOS",
        .initialize	= DAOS_Init,
        .finalize	= DAOS_Fini,
        .create		= DAOS_Create,
        .open		= DAOS_Open,
        .access		= DAOS_Access,
        .xfer		= DAOS_Xfer,
        .close		= DAOS_Close,
        .delete		= DAOS_Delete,
        .get_version	= DAOS_GetVersion,
        .xfer_hints	= DAOS_init_xfer_options,
        .fsync		= DAOS_Fsync,
        .get_file_size	= DAOS_GetFileSize,
        .statfs		= aiori_posix_statfs,
        .mkdir		= aiori_posix_mkdir,
        .rmdir		= aiori_posix_rmdir,
        .stat		= aiori_posix_stat,
	.get_options	= DAOS_options,
        .xfer_hints	= DAOS_init_xfer_options,
        .check_params	= DAOS_check_params,
        .enable_mdtest	= false,
};

#define IOR_DAOS_MUR_SEED 0xDEAD10CC

enum handleType {
        POOL_HANDLE,
        CONT_HANDLE,
	ARRAY_HANDLE
};

static daos_handle_t	poh;
static daos_handle_t	coh;
static daos_handle_t	aoh;
static daos_oclass_id_t objectClass = OC_SX;
static bool		daos_initialized = false;

/***************************** F U N C T I O N S ******************************/

/* For DAOS methods. */
#define DCHECK(rc, format, ...)                                         \
do {                                                                    \
        int _rc = (rc);                                                 \
                                                                        \
        if (_rc < 0) {                                                  \
                fprintf(stderr, "ior ERROR (%s:%d): %d: %d: "           \
                        format"\n", __FILE__, __LINE__, rank, _rc,      \
                        ##__VA_ARGS__);                                 \
                fflush(stdout);                                         \
                MPI_Abort(MPI_COMM_WORLD, -1);                          \
        }                                                               \
} while (0)

#define INFO(level, format, ...)					\
do {                                                                    \
        if (verbose >= level)						\
                printf("[%d] "format"\n", rank, ##__VA_ARGS__);         \
} while (0)

/* For generic errors like invalid command line options. */
#define GERR(format, ...)                                               \
do {                                                                    \
        fprintf(stderr, format"\n", ##__VA_ARGS__);                     \
        MPI_CHECK(MPI_Abort(MPI_COMM_WORLD, -1), "MPI_Abort() error");  \
} while (0)

static aiori_xfer_hint_t * hints = NULL;

void DAOS_init_xfer_options(aiori_xfer_hint_t * params)
{
        hints = params;
}

static int DAOS_check_params(aiori_mod_opt_t * options){
        DAOS_options_t *o = (DAOS_options_t *) options;

        if (o->pool == NULL || o->svcl == NULL || o->cont == NULL)
                ERR("Invalid pool or container options\n");

        return 0;
}

/* Distribute process 0's pool or container handle to others. */
static void
HandleDistribute(daos_handle_t *handle, enum handleType type)
{
        d_iov_t global;
        int        rc;

        global.iov_buf = NULL;
        global.iov_buf_len = 0;
        global.iov_len = 0;

        if (rank == 0) {
                /* Get the global handle size. */
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(*handle, &global);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_local2global(*handle, &global);
		else
			rc = daos_array_local2global(*handle, &global);
                DCHECK(rc, "Failed to get global handle size");
        }

        MPI_CHECK(MPI_Bcast(&global.iov_buf_len, 1, MPI_UINT64_T, 0,
                            MPI_COMM_WORLD),
                  "Failed to bcast global handle buffer size");

	global.iov_len = global.iov_buf_len;
        global.iov_buf = malloc(global.iov_buf_len);
        if (global.iov_buf == NULL)
                ERR("Failed to allocate global handle buffer");

        if (rank == 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_local2global(*handle, &global);
                else if (type == CONT_HANDLE) 
                        rc = daos_cont_local2global(*handle, &global);
		else
			rc = daos_array_local2global(*handle, &global);
                DCHECK(rc, "Failed to create global handle");
        }

        MPI_CHECK(MPI_Bcast(global.iov_buf, global.iov_buf_len, MPI_BYTE, 0,
                            MPI_COMM_WORLD),
                  "Failed to bcast global pool handle");

        if (rank != 0) {
                if (type == POOL_HANDLE)
                        rc = daos_pool_global2local(global, handle);
                else if (type == CONT_HANDLE)
                        rc = daos_cont_global2local(poh, global, handle);
		else
			rc = daos_array_global2local(coh, global, 0, handle);
                DCHECK(rc, "Failed to get local handle");
        }

        free(global.iov_buf);
}

static void
DAOS_Init(aiori_mod_opt_t * options)
{
        DAOS_options_t *o = (DAOS_options_t *)options;
        int rc;

	if (daos_initialized)
		return;

	if (o->pool == NULL || o->svcl == NULL || o->cont == NULL)
		return;

        if (o->oclass) {
                objectClass = daos_oclass_name2id(o->oclass);
		if (objectClass == OC_UNKNOWN)
			GERR("Invalid DAOS Object class %s\n", o->oclass);
	}

        rc = daos_init();
	if (rc)
		DCHECK(rc, "Failed to initialize daos");

        if (rank == 0) {
                uuid_t			uuid;
		d_rank_list_t		*svcl = NULL;
		static daos_pool_info_t po_info;
		static daos_cont_info_t co_info;

                INFO(VERBOSE_1, "Connecting to pool %s", o->pool);

                rc = uuid_parse(o->pool, uuid);
                DCHECK(rc, "Failed to parse 'pool': %s", o->pool);

		svcl = daos_rank_list_parse(o->svcl, ":");
		if (svcl == NULL)
			ERR("Failed to allocate svcl");

                rc = daos_pool_connect(uuid, o->group, svcl, DAOS_PC_RW,
				       &poh, &po_info, NULL);
		d_rank_list_free(svcl);
                DCHECK(rc, "Failed to connect to pool %s", o->pool);

                INFO(VERBOSE_1, "Create/Open Container %s", o->cont);

		uuid_clear(uuid);
		rc = uuid_parse(o->cont, uuid);
		DCHECK(rc, "Failed to parse 'cont': %s", o->cont);

		rc = daos_cont_open(poh, uuid, DAOS_COO_RW, &coh, &co_info,
				    NULL);
		/* If NOEXIST we create it */
		if (rc == -DER_NONEXIST) {
			INFO(VERBOSE_2, "Creating DAOS Container...\n");
			rc = daos_cont_create(poh, uuid, NULL, NULL);
			if (rc == 0)
				rc = daos_cont_open(poh, uuid, DAOS_COO_RW,
						    &coh, &co_info, NULL);
		}
		DCHECK(rc, "Failed to create container");
        }

        HandleDistribute(&poh, POOL_HANDLE);
        HandleDistribute(&coh, CONT_HANDLE);
	aoh.cookie = 0;

	daos_initialized = true;
}

static void
DAOS_Fini(aiori_mod_opt_t *options)
{
        DAOS_options_t *o = (DAOS_options_t *)options;
        int rc;

	if (!daos_initialized)
		return;

	MPI_Barrier(MPI_COMM_WORLD);
	rc = daos_cont_close(coh, NULL);
	if (rc) {
		DCHECK(rc, "Failed to close container %s (%d)", o->cont, rc);
		MPI_Abort(MPI_COMM_WORLD, -1);
	}
	MPI_Barrier(MPI_COMM_WORLD);

	if (o->destroy) {
		if (rank == 0) {
			uuid_t uuid;
			double t1, t2;

			INFO(VERBOSE_1, "Destroying DAOS Container %s", o->cont);
			uuid_parse(o->cont, uuid);
			t1 = MPI_Wtime();
			rc = daos_cont_destroy(poh, uuid, 1, NULL);
			t2 = MPI_Wtime();
			if (rc == 0)
				INFO(VERBOSE_1, "Container Destroy time = %f secs", t2-t1);
		}

		MPI_Bcast(&rc, 1, MPI_INT, 0, MPI_COMM_WORLD);
		if (rc) {
			if (rank == 0)
				DCHECK(rc, "Failed to destroy container %s (%d)", o->cont, rc);
			MPI_Abort(MPI_COMM_WORLD, -1);
		}
	}

        if (rank == 0)
		INFO(VERBOSE_1, "Disconnecting from DAOS POOL..");

	rc = daos_pool_disconnect(poh, NULL);
	DCHECK(rc, "Failed to disconnect from pool %s", o->pool);

	MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD), "barrier error");
        if (rank == 0)
		INFO(VERBOSE_1, "Finalizing DAOS..");

        rc = daos_fini();
        DCHECK(rc, "Failed to finalize daos");

	daos_initialized = false;
}

static void
gen_oid(const char *name, daos_obj_id_t *oid)
{
	oid->lo = d_hash_murmur64(name, strlen(name), IOR_DAOS_MUR_SEED);
	oid->hi = 0;

	daos_array_generate_id(oid, objectClass, true, 0);
}

static aiori_fd_t *
DAOS_Create(char *testFileName, int flags, aiori_mod_opt_t *param)
{
        DAOS_options_t *o = (DAOS_options_t*) param;
	daos_obj_id_t	oid;
	int		rc;

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);
               
	/** Create the array */
	if (hints->filePerProc || rank == 0) {
		rc = daos_array_create(coh, oid, DAOS_TX_NONE, 1, o->chunk_size,
				       &aoh, NULL);
		DCHECK(rc, "Failed to create array object\n");
	}

	/** Distribute the array handle if not FPP */
	if (!hints->filePerProc)
		HandleDistribute(&aoh, ARRAY_HANDLE);

	return (aiori_fd_t*)(&aoh);
}

static int
DAOS_Access(const char *testFileName, int mode, aiori_mod_opt_t * param)
{
	daos_obj_id_t	oid;
	daos_size_t	cell_size, chunk_size;
	int		rc;

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	rc = daos_array_open(coh, oid, DAOS_TX_NONE, DAOS_OO_RO,
			     &cell_size, &chunk_size, &aoh, NULL);
	if (rc)
		return rc;

	if (cell_size != 1)
		GERR("Invalid DAOS Array object.\n");

	rc = daos_array_close(aoh, NULL);
	aoh.cookie = 0;
	return rc;
}

static aiori_fd_t *
DAOS_Open(char *testFileName, int flags, aiori_mod_opt_t *param)
{
	daos_obj_id_t	oid;

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	/** Open the array */
	if (hints->filePerProc || rank == 0) {
		daos_size_t cell_size, chunk_size;
		int rc;

		rc = daos_array_open(coh, oid, DAOS_TX_NONE, DAOS_OO_RW,
				     &cell_size, &chunk_size, &aoh, NULL);
		DCHECK(rc, "Failed to create array object\n");

		if (cell_size != 1)
			GERR("Invalid DAOS Array object.\n");
	}

	/** Distribute the array handle if not FPP */
	if (!hints->filePerProc)
		HandleDistribute(&aoh, ARRAY_HANDLE);

	return (aiori_fd_t*)(&aoh);
}

static IOR_offset_t
DAOS_Xfer(int access, aiori_fd_t *file, IOR_size_t *buffer, IOR_offset_t length,
	  IOR_offset_t off, aiori_mod_opt_t *param)
{
	daos_array_iod_t        iod;
	daos_range_t            rg;
	d_sg_list_t		sgl;
	d_iov_t			iov;
	int			rc;

	/** set array location */
	iod.arr_nr = 1;
	rg.rg_len = length;
	rg.rg_idx = off;
	iod.arr_rgs = &rg;

	/** set memory location */
	sgl.sg_nr = 1;
	d_iov_set(&iov, buffer, length);
	sgl.sg_iovs = &iov;

        if (access == WRITE) {
		rc = daos_array_write(aoh, DAOS_TX_NONE, &iod, &sgl, NULL);
                DCHECK(rc, "daos_array_write() failed (%d).", rc);
	} else {
		rc = daos_array_read(aoh, DAOS_TX_NONE, &iod, &sgl, NULL);
                DCHECK(rc, "daos_array_read() failed (%d).", rc);
	}

	return length;
}

static void
DAOS_Close(aiori_fd_t *file, aiori_mod_opt_t *param)
{
        int rc;

	if (!daos_initialized)
		GERR("DAOS is not initialized!");

	rc = daos_array_close(aoh, NULL);
	DCHECK(rc, "daos_array_close() failed (%d).", rc);

	aoh.cookie = 0;
}

static void
DAOS_Delete(char *testFileName, aiori_mod_opt_t *param)
{
	daos_obj_id_t	oid;
	daos_size_t	cell_size, chunk_size;
        int		rc;

	if (!daos_initialized)
		GERR("DAOS is not initialized!");

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	/** open the array to verify it exists */
	rc = daos_array_open(coh, oid, DAOS_TX_NONE, DAOS_OO_RW,
			     &cell_size, &chunk_size, &aoh, NULL);
	DCHECK(rc, "daos_array_open() failed (%d).", rc);

	if (cell_size != 1)
		GERR("Invalid DAOS Array object.\n");

	rc = daos_array_destroy(aoh, DAOS_TX_NONE, NULL);
	DCHECK(rc, "daos_array_destroy() failed (%d).", rc);

	rc = daos_array_close(aoh, NULL);
	DCHECK(rc, "daos_array_close() failed (%d).", rc);
	aoh.cookie = 0;
}

static char *
DAOS_GetVersion()
{
	static char ver[1024] = {};

	sprintf(ver, "%s", "DAOS");
	return ver;
}

static void
DAOS_Fsync(aiori_fd_t *file, aiori_mod_opt_t *param)
{
	return;
}

static IOR_offset_t
DAOS_GetFileSize(aiori_mod_opt_t *param, char *testFileName)
{
	daos_obj_id_t	oid;
	daos_size_t	size;
        int		rc;

	if (!daos_initialized)
		GERR("DAOS is not initialized!");

	/** Convert file name into object ID */
	gen_oid(testFileName, &oid);

	/** open the array to verify it exists */
	if (hints->filePerProc || rank == 0) {
		daos_size_t cell_size, chunk_size;

		rc = daos_array_open(coh, oid, DAOS_TX_NONE, DAOS_OO_RO,
				     &cell_size, &chunk_size, &aoh, NULL);
		DCHECK(rc, "daos_array_open() failed (%d).", rc);

		if (cell_size != 1)
			GERR("Invalid DAOS Array object.\n");

		rc = daos_array_get_size(aoh, DAOS_TX_NONE, &size, NULL);
		DCHECK(rc, "daos_array_get_size() failed (%d).", rc);

		rc = daos_array_close(aoh, NULL);
		DCHECK(rc, "daos_array_close() failed (%d).", rc);
		aoh.cookie = 0;
	}

	if (!hints->filePerProc)
		MPI_Bcast(&size, 1, MPI_LONG, 0, MPI_COMM_WORLD);

	return size;
}
