/* -*- mode: C; c-basic-offset: 4  -*- */
/* ex: set shiftwidth=4 expandtab: */
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

/** Author: Neil Dantam
 */

#include <argp.h>
#include <syslog.h>
#include <sns.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>
#include <gamepad.h>
#include <amino.h>
#include <reflex.h>
#include "piranha.h"

enum pir_mode {
    MODE_HALT = 0,
    MODE_TORSO = 1,
    MODE_L_SHOULDER = 2,
    MODE_L_WRIST = 3,
    MODE_R_SHOULDER = 4,
    MODE_R_WRIST = 5,
};

struct pir_msg {
    char mode[64];
    int64_t i;
};

typedef struct {
    ach_channel_t chan_js;
    ach_channel_t chan_ref_torso;
    ach_channel_t chan_ref_left;
    ach_channel_t chan_state_pir;
    ach_channel_t chan_ctrl;
    struct sns_msg_joystick *jsmsg;
    struct sns_msg_motor_ref *msg_ref;
    struct pir_msg msg_ctrl;
    struct pir_state state;
    struct {
        double  q[PIR_AXIS_CNT];
        double dq[PIR_AXIS_CNT];
    } ref;
    struct timespec now;
    rfx_ctrl_t G;
    rfx_ctrl_ws_lin_k_t K;
    double q_min[PIR_AXIS_CNT];
    double q_max[PIR_AXIS_CNT];
} cx_t;

cx_t cx;



// 20 deg/s
#define MAXVEL_FACTOR 20 * M_PI/180

#define JS_AXES 8

#define VALID_NS (1000000000 / 5)

static void set_mode(void);
static void update(void);
static void control(void);

// all the different control modes
typedef void (*ctrl_fun_t)(void);
static void ctrl_joint_left_shoulder(void);
static void ctrl_joint_left_wrist(void);
static void ctrl_ws_left(void);
static void control_n( uint32_t n, size_t i, ach_channel_t *chan );

static const double tf_ident[] = {1,0,0, 0,1,0, 0,0,1, 0,0,0};

int main( int argc, char **argv ) {
    memset(&cx, 0, sizeof(cx));
    memcpy( cx.state.Tee, tf_ident, 12*sizeof(cx.state.Tee[0]) );
    memcpy( cx.state.T0, tf_ident, 12*sizeof(cx.state.T0[0]) );

    /*-- args --*/
    for( int c; -1 != (c = getopt(argc, argv, "V?hH" SNS_OPTSTRING)); ) {
        switch(c) {
            SNS_OPTCASES;
        default:
            sns_die( 0, "Invalid argument: %s\n", optarg );
        }
    }

    sns_start();

    // open channel
    sns_chan_open( &cx.chan_js,          "joystick",    NULL );
    sns_chan_open( &cx.chan_ctrl,        "pir-ctrl",    NULL );
    sns_chan_open( &cx.chan_ref_torso,   "ref-torso",   NULL );
    sns_chan_open( &cx.chan_ref_left,    "ref-left",    NULL );
    sns_chan_open( &cx.chan_state_pir,   "pir-state",   NULL );

    // alloc messages
    cx.msg_ref = sns_msg_motor_ref_alloc( PIR_MAX_MSG_AXES );
    cx.jsmsg = sns_msg_joystick_alloc( JS_AXES );
    cx.msg_ref->mode = SNS_MOTOR_MODE_VEL;

    // setup reflex controller
    rfx_ctrl_ws_lin_k_init( &cx.K, 7 );
    cx.G.n_q = 7;
    cx.G.J =  cx.state.J;
    cx.G.q =  &cx.state.q[PIR_AXIS_L0];
    cx.G.dq = &cx.state.dq[PIR_AXIS_L0];
    cx.G.q_r =  &cx.ref.q[PIR_AXIS_L0];
    cx.G.dq_r = &cx.ref.dq[PIR_AXIS_L0];
    cx.G.q_min = &cx.q_min[PIR_AXIS_L0];
    cx.G.q_max = &cx.q_max[PIR_AXIS_L0];
    for( size_t i = 0; i < PIR_AXIS_CNT; i ++ ) {
        cx.q_min[i] = -2*M_PI;
        cx.q_max[i] = M_PI;
    }
    for( size_t i = 0; i < 3; i ++ ) {
        cx.G.x_min[i] = -10;
        cx.G.x_max[i] = 10;
    }
    aa_fset( cx.K.q, 0.1, 7 );
    aa_fset( cx.K.f, 0, 6 );
    aa_fset( cx.K.p, 0.5, 3 );
    aa_fset( cx.K.p+3, 0.5, 3 );
    cx.K.dls = .000;


    if( clock_gettime( ACH_DEFAULT_CLOCK, &cx.now ) )
        SNS_LOG( LOG_ERR, "clock_gettime failed: '%s'\n", strerror(errno) );

    /* -- RUN -- */
    while (!sns_cx.shutdown) {

        // get state
        update();

        //if( clock_gettime( ACH_DEFAULT_CLOCK, &cx.now ) )
            //SNS_LOG( LOG_ERR, "clock_gettime failed: '%s'\n", strerror(errno) );

        if( SNS_LOG_PRIORITY( LOG_DEBUG + 1 ) ) {
            sns_msg_joystick_dump( stderr, cx.jsmsg );
        }

        // control
        control();

        cx.now = sns_time_add_ns(cx.now, 1000*1000*10 );
        clock_nanosleep( ACH_DEFAULT_CLOCK, TIMER_ABSTIME,
                         &cx.now, NULL );
    }

    sns_end();
    return 0;
}

static void update(void) {

    // state
    {
        size_t frame_size;
        //struct timespec timeout = sns_time_add_ns( cx.now, 1000*1000*10 );
        ach_status_t r = ach_get( &cx.chan_state_pir, &cx.state, sizeof(cx.state), &frame_size,
                                  NULL, ACH_O_LAST );
        SNS_REQUIRE( ( (ACH_OK == r || ACH_MISSED_FRAME == r) && frame_size == sizeof(cx.state) ) ||
                     ACH_TIMEOUT == r ||
                     ACH_STALE_FRAMES == r,
                     "Failed to get frame: %s\n", ach_result_to_string(r) );
    }


    // joystick
    {
        size_t frame_size;
        size_t max_size = sns_msg_joystick_size_n( JS_AXES );
        // get joystick
        ach_status_t r = ach_get( &cx.chan_js, cx.jsmsg, max_size, &frame_size,
                                  NULL,  ACH_O_LAST );

        SNS_REQUIRE( ( ACH_OK == r || ACH_MISSED_FRAME == r || ACH_STALE_FRAMES == r ),
                     "Failed to get frame: %s\n", ach_result_to_string(r) );

        // validate
        if( (ACH_TIMEOUT == r || ACH_STALE_FRAMES == r) ) {
            ;// pass
        } else if( (ACH_OK == r || ACH_MISSED_FRAME == r) &&
                   (cx.jsmsg->n != JS_AXES ||
                    frame_size != sns_msg_joystick_size(cx.jsmsg) ) )
        {
            printf("bad js\n");
            memset( cx.jsmsg->axis, 0, sizeof(cx.jsmsg->axis[0])*JS_AXES );
            cx.jsmsg->buttons = 0;
        } else {
            if( cx.jsmsg->buttons & GAMEPAD_BUTTON_BACK ) {
                if( strcmp(cx.msg_ctrl.mode, "halt") ) {
                    printf("HALT\n");
                }
                strcpy( cx.msg_ctrl.mode, "halt" );
            }
        }
    }


    //mode
    set_mode();
}


static void set_mode(void) {
    // poll mode
    size_t frame_size;
    ach_status_t r = ach_get( &cx.chan_ctrl, &cx.msg_ctrl, sizeof(cx.msg_ctrl), &frame_size,
                 NULL, ACH_O_LAST );
    if( ACH_OK == r || ACH_MISSED_FRAME == r ) {
        cx.msg_ctrl.mode[63] = '\0';
        printf("ctrl: %s %"PRId64"\n", cx.msg_ctrl.mode, cx.msg_ctrl.i);

        if( 0 == strcmp("ws-left", cx.msg_ctrl.mode ) ) {
            memcpy( cx.G.x_r, &cx.state.T[9], sizeof(cx.G.x_r[0]) * 3 );
            aa_tf_rotmat2quat( cx.state.T, cx.G.r_r );

        }
    }
}

static void control(void) {
    // dispatch
    memset( cx.ref.dq, 0, sizeof(cx.ref.dq[0])*PIR_AXIS_CNT );
    static const struct  {
        const char *name;
        ctrl_fun_t fun;
    } cmds[] = {
        {"teleop-left-shoulder", ctrl_joint_left_shoulder},
        {"teleop-left-wrist", ctrl_joint_left_wrist},
        {"ws-left", ctrl_ws_left},
        {NULL, NULL} };

    for( size_t i = 0; cmds[i].name != NULL; i ++ ) {
        if( 0 == strcmp(cx.msg_ctrl.mode, cmds[i].name) ) {
            cmds[i].fun();
            break;
        }
    }
    // send ref
    sns_msg_set_time( &cx.msg_ref->header, &cx.now, VALID_NS );
    // torso
    control_n( 1, PIR_AXIS_T, &cx.chan_ref_torso );
    // left
    control_n( 7, PIR_AXIS_L0, &cx.chan_ref_left );
}

static void control_n( uint32_t n, size_t i, ach_channel_t *chan ) {
    memcpy( &cx.msg_ref->u[0], &cx.ref.dq[i], sizeof(cx.msg_ref->u[0])*n );
    cx.msg_ref->mode = SNS_MOTOR_MODE_VEL;
    cx.msg_ref->n = n;
    ach_put( chan, cx.msg_ref, sns_msg_motor_ref_size(cx.msg_ref) );
    // TODO: check result
}



static void ctrl_joint_torso(void) {
    double u = cx.jsmsg->axis[GAMEPAD_AXIS_RT] - cx.jsmsg->axis[GAMEPAD_AXIS_LT];
    cx.ref.dq[PIR_AXIS_T] = u * MAXVEL_FACTOR;
}
static void ctrl_joint_left_shoulder(void) {
    ctrl_joint_torso();
    cx.ref.dq[PIR_AXIS_L0] = cx.jsmsg->axis[GAMEPAD_AXIS_LY] * MAXVEL_FACTOR;
    cx.ref.dq[PIR_AXIS_L1] = cx.jsmsg->axis[GAMEPAD_AXIS_LX] * MAXVEL_FACTOR;
    cx.ref.dq[PIR_AXIS_L2] = cx.jsmsg->axis[GAMEPAD_AXIS_RY] * MAXVEL_FACTOR;
    cx.ref.dq[PIR_AXIS_L3] = cx.jsmsg->axis[GAMEPAD_AXIS_RX] * MAXVEL_FACTOR;
}
static void ctrl_joint_left_wrist(void) {
    ctrl_joint_torso();
    cx.ref.dq[PIR_AXIS_L3] = cx.jsmsg->axis[GAMEPAD_AXIS_LX] * MAXVEL_FACTOR;
    cx.ref.dq[PIR_AXIS_L4] = cx.jsmsg->axis[GAMEPAD_AXIS_LY] * MAXVEL_FACTOR;
    cx.ref.dq[PIR_AXIS_L5] = cx.jsmsg->axis[GAMEPAD_AXIS_RX] * MAXVEL_FACTOR;
    cx.ref.dq[PIR_AXIS_L6] = cx.jsmsg->axis[GAMEPAD_AXIS_RY] * MAXVEL_FACTOR;
}


static void ctrl_ws_left(void) {
    // set actuals
    memcpy( cx.G.x, &cx.state.T[9], sizeof(cx.G.x[0]) * 3 );
    aa_tf_rotmat2quat( cx.state.T, cx.G.r );

    // set refs
    cx.G.dx_r[0] = cx.jsmsg->axis[GAMEPAD_AXIS_LX] * .05;
    cx.G.dx_r[1] = cx.jsmsg->axis[GAMEPAD_AXIS_LY] * .05;
    cx.G.dx_r[2] = cx.jsmsg->axis[GAMEPAD_AXIS_RX] * .05;

    // compute stuff
    int r = rfx_ctrl_ws_lin_vfwd( &cx.G,
                                  &cx.K,
                                  &cx.ref.dq[PIR_AXIS_L0] );
    if( RFX_OK != r ) {
        SNS_LOG( LOG_ERR, "ws error: %s\n",
                 rfx_status_string((rfx_status_t)r) );
    }

    // integrate
    /* double xr[3]; */
    /* aa_tf_quat2rotvec( cx->X.arm[KRANG_I_LEFT].G.r_r, xr ); */
    /* aa_la_axpy(3, dt, cx->X.arm[KRANG_I_LEFT].G.dx_r, */
    /*            cx->X.arm[KRANG_I_LEFT].G.x_r ); */
    /* aa_la_axpy(3, dt, cx->X.arm[KRANG_I_LEFT].G.dx_r + 3, */
    /*            xr ); */
    /* aa_tf_rotvec2quat( xr, cx->X.arm[KRANG_I_LEFT].G.r_r ); */

}
