/* -*- mode: C; c-basic-offset: 4 -*- */
/* ex: set shiftwidth=4 tabstop=4 expandtab: */
/*
 * Copyright (c) 2013, Georgia Tech Research Corporation
 * All rights reserved.
 *
 * Author(s): Neil T. Dantam <ntd@gatech.edu>
 * Georgia Tech Humanoid Robotics Lab
 * Under Direction of Prof. Mike Stilman <mstilman@cc.gatech.edu>
 *
 *
 * This file is provided under the following "BSD-style" License:
 *
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <amino.h>
#include <sns.h>
#include <getopt.h>
#include <reflex.h>
#include "piranha.h"

#define N_MARKERS 32

//const char *opt_file_cam = NULL;
//const char *opt_file_fk = NULL;
//const char *opt_file_out = NULL;
//size_t opt_test = 0;
//int opt_verbosity = 0;
//double opt_d_theta = 0;
//double opt_d_x = 0;

const char *opt_file_config = NULL;
const char *opt_file_marker = NULL;

const char *opt_file_cam = NULL;
const char *opt_file_fk = NULL;

double opt_wt_thresh = 0;

int opt_run = 0;
int opt_comp = 0;


static void
run_cal( void );

static void
compute_cal( void );

int main( int argc, char **argv )
{
    /* Parse */
    for( int c; -1 != (c = getopt(argc, argv, "q:m:RC")); ) {
        switch(c) {
        case 'q':
            opt_file_config = optarg;
            break;
        case 'm':
            opt_file_marker = optarg;
            break;
        case 'c':
            opt_file_cam = optarg;
            break;
        case 'k':
            opt_file_fk = optarg;
            break;
        case 'R':
            opt_run = 1;
            break;
        case 'C':
            opt_comp = 1;
            break;
        case '?':   /* help     */
            puts( "Usage: rfx-camcal -k FK_POSE_FILE -c CAM_POSE_FILE \n"
                  "Calibrate a camera from list of kinematics and camera transforms"
                  "\n"
                  "Options:\n"
                  "  -q CONFIG-FILE,             Config file\n"
                  "  -m MARKER-FILE,             Marker file\n"
                  "  -c CAM-FILE,                Marker Pose file\n"
                  "  -k FK-FILE,                 FK Pose file\n"
                  "  -R,                         Run a calibration\n"
                  "  -C,                         Compute a calibration\n"
                  "\n"
                  "Report bugs to <ntd@gatech.edu>"
                );
            exit(EXIT_SUCCESS);
            break;
        default:
            printf("Unknown argument: `%s'\n", optarg);
            exit(EXIT_FAILURE);
        }
    }
    if( opt_run ) {
        run_cal();
    }

    if( opt_comp ) {
        compute_cal();
    }

    return 0;
}

static void
run_cal( void )
{
    sns_init();
    ach_channel_t chan_config, chan_marker;
    sns_chan_open( &chan_config, "pir-config", NULL );
    sns_chan_open( &chan_marker, "pir-marker", NULL );

    FILE *f_q = fopen( opt_file_config, "w" );
    SNS_REQUIRE( NULL != f_q, "Error opening %s\n", opt_file_config );

    FILE *f_m = fopen( opt_file_marker, "w" );
    SNS_REQUIRE( NULL != f_m, "Error opening %s\n", opt_file_marker );

    char *lineptr = NULL;
    size_t n = 0;
    while( -1 != (getline(&lineptr, &n, stdin)) &&
           0 == strcmp(".", lineptr) )
    {
        // get config
        {
            size_t frame_size;
            double *config;
            ach_status_t r = sns_msg_local_get( &chan_config, (void**)&config,
                                                &frame_size, NULL, ACH_O_WAIT | ACH_O_LAST );
            SNS_REQUIRE( r == ACH_OK || r == ACH_MISSED_FRAME,
                         "Error getting config: %s\n", ach_result_to_string(r) );
            size_t expected_size = 2*PIR_TF_CONFIG_MAX*sizeof(double);
            SNS_REQUIRE( expected_size == frame_size,
                         "Unexpected frame size: saw %lu, wanted %lu\n",
                         frame_size, expected_size );

            aa_dump_vec( f_q, config, PIR_TF_CONFIG_MAX );

            // get marker
        }
        // get marker
        {
            size_t frame_size;
            struct sns_msg_wt_tf *wt_tf;
            ach_status_t r = sns_msg_wt_tf_local_get( &chan_config, &wt_tf,
                                                      &frame_size, NULL, ACH_O_WAIT | ACH_O_LAST );
            SNS_REQUIRE( r == ACH_OK || r == ACH_MISSED_FRAME,
                         "Error getting markers: %s\n", ach_result_to_string(r) );
            SNS_REQUIRE( frame_size > sizeof( sns_msg_header_t ) &&
                         frame_size == sns_msg_wt_tf_size(wt_tf),
                         "Invalid wt_tf message size\n" );

            aa_dump_vec( f_m, (double*)&(wt_tf->wt_tf[0]), wt_tf->header.n*8 );
        }

        // write data
        aa_mem_region_local_release();
    }
    fclose(f_q);
    fclose(f_m);
}


struct marker_pair {
    int fk_frame;
    int marker_id;
};

int marker2frame( size_t marker_id ) {
    switch(marker_id) {
        // TODO: find correspondences
    /*     /\* Left *\/ */
    /* case -1: return PIR_TF_LEFT_SDH_L_K0M; */
    //case 0: return PIR_TF_LEFT_SDH_L_K0P;
    /* case -1: return PIR_TF_LEFT_SDH_L_K1M; */
    /* case -1: return PIR_TF_LEFT_SDH_L_K1P; */

    /*     // T */
    /* case -1: return PIR_TF_LEFT_SDH_T_K0M; */
    /* case -1: return PIR_TF_LEFT_SDH_T_K0P; */
    /* case -1: return PIR_TF_LEFT_SDH_T_K1M; */
    /* case -1: return PIR_TF_LEFT_SDH_T_K1P; */

    /*     // R */
    //case 1: return PIR_TF_LEFT_SDH_R_K0M;
    /* case -1: return PIR_TF_LEFT_SDH_R_K0P; */
    /* case -1: return PIR_TF_LEFT_SDH_R_K1M; */
    //case 9: return PIR_TF_LEFT_SDH_R_K1P;

    /*     /\* Right *\/ */
    /*     // L */
    /* case -1: return PIR_TF_RIGHT_SDH_L_K0M; */
    case 0: return PIR_TF_RIGHT_SDH_L_K0P;
    /* case -1: return PIR_TF_RIGHT_SDH_L_K1M; */
    /* case -1: return PIR_TF_RIGHT_SDH_L_K1P; */

    /*     // T */
    /* case -1: return PIR_TF_RIGHT_SDH_T_K0M; */
    /* case -1: return PIR_TF_RIGHT_SDH_T_K0P; */
    /* case -1: return PIR_TF_RIGHT_SDH_T_K1M; */
    /* case -1: return PIR_TF_RIGHT_SDH_T_K1P; */

    /*     // R */
    case 1: return PIR_TF_RIGHT_SDH_R_K0M;
    /* case -1: return PIR_TF_RIGHT_SDH_R_K0P; */
    case 9: return PIR_TF_RIGHT_SDH_R_K1M;
    /* case -1: return PIR_TF_RIGHT_SDH_R_K1P; */

    default: return -1;
    }
}

static void
compute_cal( void )
{
    // open files
    FILE *f_q = fopen( opt_file_config, "r" );
    SNS_REQUIRE( NULL != f_q, "Error opening %s\n", opt_file_config );

    FILE *f_m = fopen( opt_file_marker, "r" );
    SNS_REQUIRE( NULL != f_m, "Error opening %s\n", opt_file_marker );

    FILE *f_c = fopen( opt_file_cam, "w" );
    SNS_REQUIRE( NULL != f_c, "Error opening %s\n", opt_file_cam );

    FILE *f_k = fopen( opt_file_fk, "w" );
    SNS_REQUIRE( NULL != f_k, "Error opening %s\n", opt_file_fk );

    double *Q, *M;
    size_t lines;
    size_t marker_elts = N_MARKERS * sizeof(struct sns_msg_wt_tf)/sizeof(double);
    {
        ssize_t lines_q = aa_io_fread_matrix_heap( f_q, PIR_TF_CONFIG_MAX, &Q, NULL);
        ssize_t lines_m = aa_io_fread_matrix_heap( f_m, marker_elts,
                                                   &M, NULL);
        SNS_REQUIRE( lines_q > 0 && lines_m > 0 && lines_q == lines_m,
                     "Error reading files\n" );
        lines = (size_t)lines_q;
    }

    // loop through lines and write correspondenses
    for( size_t i = 0; i < lines; i ++ )
    {
        double *q = &Q[i*PIR_TF_CONFIG_MAX];
        sns_wt_tf *wt_tf = (sns_wt_tf*) &M[i*marker_elts];

        double *tf_rel = (double*)aa_mem_region_local_alloc( 7 * PIR_TF_FRAME_MAX * sizeof(tf_rel[0]) );
        double *tf_abs = (double*)aa_mem_region_local_alloc( 7 * PIR_TF_FRAME_MAX * sizeof(tf_abs[0]) );
        pir_tf_rel( q, tf_rel );
        pir_tf_abs( tf_rel, tf_abs );

        for( size_t j = 0; j < N_MARKERS; j ++ ) {
            ssize_t frame = marker2frame(j);
            if( frame > 0 && wt_tf[j].weight > opt_wt_thresh ) {
                aa_dump_vec( f_c, wt_tf[j].tf.data, 7);
                aa_dump_vec( f_k, &tf_abs[7*frame], 7);
            }
        }

    }

    SNS_REQUIRE( feof(f_q) && feof(f_m),
                 "Error reading marker and config files\n" );
}


/* static struct marker_pair pairs[] = */
/* { */
/*     /\** Left **\/ */
/*     // L */
/*     {.fk_frame=PIR_TF_LEFT_SDH_L_K0M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_L_K0P, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_L_K1M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_L_K1P, .marker_id=-1}, */

/*     // T */
/*     {.fk_frame=PIR_TF_LEFT_SDH_T_K0M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_T_K0P, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_T_K1M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_T_K1P, .marker_id=-1}, */

/*     // R */
/*     {.fk_frame=PIR_TF_LEFT_SDH_T_R0M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_T_R0P, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_T_R1M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_LEFT_SDH_T_R1P, .marker_id=-1}, */

/*     /\** Right **\/ */
/*     // L */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_L_K0M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_L_K0P, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_L_K1M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_L_K1P, .marker_id=-1}, */

/*     // T */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_T_K0M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_T_K0P, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_T_K1M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_T_K1P, .marker_id=-1}, */

/*     // R */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_T_R0M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_T_R0P, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_T_R1M, .marker_id=-1}, */
/*     {.fk_frame=PIR_TF_RIGHT_SDH_T_R1P, .marker_id=-1}, */
/* }; */