/**
 * Copyright 2018, Decawave Limited, All Rights Reserved
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <os/os.h>
#include <hal/hal_spi.h>
#include <hal/hal_gpio.h>
#include "bsp/bsp.h"


#include <dw1000/dw1000_regs.h>
#include <dw1000/dw1000_dev.h>
#include <dw1000/dw1000_hal.h>
#include <dw1000/dw1000_mac.h>
#include <dw1000/dw1000_phy.h>
#include <dw1000/dw1000_ftypes.h>

#if MYNEWT_VAL(DW1000_CLOCK_CALIBRATION)
#include <dw1000/dw1000_ccp.h>

static ccp_frame_t frames[] = {
    [0] = {
        .fctrl = FCNTL_IEEE_BLINK_CCP_64,    // frame control (FCNTL_IEEE_BLINK_64 to indicate a data frame using 16-bit addressing).
        .seq_num = 0xFE,
        .correction_factor = 1.0f
    },
    [1] = {
        .fctrl = FCNTL_IEEE_BLINK_CCP_64,    // frame control (FCNTL_IEEE_BLINK_64 to indicate a data frame using 16-bit addressing).
        .seq_num = 0xFF,
        .correction_factor = 1.0f
    }
};

static void ccp_rx_complete_cb(dw1000_dev_instance_t * inst);
static void ccp_tx_complete_cb(dw1000_dev_instance_t * inst);
static dw1000_ccp_status_t dw1000_ccp_blink(dw1000_dev_instance_t * inst, dw1000_dev_modes_t mode);
static void ccp_postprocess(struct os_event * ev);
static struct os_callout ccp_callout_timer;
static struct os_callout ccp_callout_postprocess;

/*! 
 * @fn ccp_timer_ev_cb(struct os_event *ev)
 *
 * @brief The OS scheduler is not accurate enough for the timing requirement of an RTLS system.  
 * Instead, the OS is used to schedule events in advance of the actual event.  
 * The DW1000 delay start mechanism then takes care of the actual event. This removes the non-deterministic 
 * latencies of the OS implementation.  
 * 
 * input parameters
 * @param inst - struct os_event *
 *    
 * output parameters
 *
 * returns none
 */
static void 
ccp_timer_ev_cb(struct os_event *ev) {
    assert(ev != NULL);
    assert(ev->ev_arg != NULL);

    dw1000_dev_instance_t * inst = (dw1000_dev_instance_t *)ev->ev_arg;
    dw1000_ccp_instance_t * ccp = inst->ccp; 
    if(dw1000_ccp_blink(inst, DWT_BLOCKING).start_tx_error)
      os_callout_reset(&ccp_callout_timer, OS_TICKS_PER_SEC * (ccp->period - MYNEWT_VAL(OS_LATENCY)) * 1e-6);
}

/*! 
 * @fn ccp_timer_init(dw1000_dev_instance_t * inst)
 *
 * @brief The default eventq is used. 
 * 
 * input parameters
 * @param inst - dw1000_dev_instance_t * 
 *    
 * output parameters
 *
 * returns none
 */
static void 
ccp_timer_init(dw1000_dev_instance_t * inst) {
    os_callout_init(&ccp_callout_timer, os_eventq_dflt_get(), ccp_timer_ev_cb, (void *) inst);
    os_callout_reset(&ccp_callout_timer, OS_TICKS_PER_SEC/100);
    dw1000_ccp_instance_t * ccp = inst->ccp; 
    ccp->status.timer_enabled = true;
}


/*! 
 * @fn dw1000_ccp_init(dw1000_dev_instance_t * inst, uint16_t nframes, uint64_t clock_master)
 *
 * @brief Precise timing is achieved by adding a fixed period to the transmission time of the previous frame. 
 * This approach solves the non-deterministic latencies caused by the OS. The OS, however, is used to schedule 
 * the next transmission event, but the DW1000 controls the actual next transmission time using the dw1000_set_delay_start.
 * This function allocates all the required resources. In the case of a large scale deployment multiple instances 
 * can be uses to track multiple clock domains. 
 * 
 * input parameters
 * @param inst - dw1000_dev_instance_t * 
 * @param nframes - Nominally set to 2 frames for the simple use case. But depending on the interpolation 
 * algorithm this should be set accordingly. For example, four frames are required or bicubic interpolation. 
 * @param clock_master - UUID address of the system clock_master all other masters are rejected.  
 * output parameters
 *
 * returns dw1000_ccp_instance_t * 
 */
dw1000_ccp_instance_t * 
dw1000_ccp_init(dw1000_dev_instance_t * inst, uint16_t nframes, uint64_t clock_master){
    assert(inst);

    if (inst->ccp == NULL ) {
        inst->ccp = (dw1000_ccp_instance_t *) malloc(sizeof(dw1000_ccp_instance_t) + nframes * sizeof(ccp_frame_t *)); 
        assert(inst->ccp);
        memset(inst->ccp, 0, sizeof(dw1000_ccp_instance_t));
        inst->ccp->status.selfmalloc = 1;
        inst->ccp->nframes = nframes; 
    }else{
        assert(inst->ccp->nframes == nframes);
    }
    inst->ccp->parent = inst;
    inst->ccp->period = MYNEWT_VAL(CCP_PERIOD);
    inst->ccp->config = (dw1000_ccp_config_t){
        .postprocess = false,
    };
    inst->clock_master = clock_master;

    os_error_t err = os_sem_init(&inst->ccp->sem, 0x1); 
    assert(err == OS_OK);

    for (uint16_t i = 0; i < inst->ccp->nframes; i++)
        inst->ccp->frames[i] = &frames[i];

    dw1000_ccp_set_callbacks(inst, ccp_rx_complete_cb, ccp_tx_complete_cb);
    dw1000_ccp_set_postprocess(inst, &ccp_postprocess);
    dw1000_ccp_instance_t * ccp = inst->ccp; 
    ccp_frame_t * frame = ccp->frames[(ccp->idx)%ccp->nframes]; 
    frame->transmission_timestamp = dw1000_read_systime(inst);
    inst->ccp->status.initialized = 1;
    return inst->ccp;
}

/*! 
 * @fn dw1000_ccp_free(dw1000_dev_instance_t * inst)
 *
 * @brief Deconstructor
 * 
 * input parameters
 * @param inst - dw1000_dev_instance_t * 
 *
 * returns none
 */
void 
dw1000_ccp_free(dw1000_ccp_instance_t * inst){
    assert(inst);  
    if (inst->status.selfmalloc)
        free(inst);
    else
        inst->status.initialized = 0;
}

void 
dw1000_ccp_set_callbacks(dw1000_dev_instance_t * inst, dw1000_dev_cb_t ccp_rx_complete_cb, dw1000_dev_cb_t ccp_tx_complete_cb){
    inst->ccp_rx_complete_cb = ccp_rx_complete_cb;
    inst->ccp_tx_complete_cb = ccp_tx_complete_cb;
}


/*! 
 * @fn dw1000_ccp_set_postprocess(dw1000_dev_instance_t * inst, os_event_fn * ccp_postprocess)
 *
 * @briefOverrides the default post-processing behaviors, replacing the JSON stream with an alternative 
 * or an advanced timescale processing algorithm.
 * 
 * input parameters
 * @param inst - dw1000_dev_instance_t * 
 *
 * returns none
 */
void 
dw1000_ccp_set_postprocess(dw1000_dev_instance_t * inst, os_event_fn * ccp_postprocess){
    os_callout_init(&ccp_callout_postprocess, os_eventq_dflt_get(), ccp_postprocess, (void *) inst);
    dw1000_ccp_instance_t * ccp = inst->ccp; 
    ccp->config.postprocess = true;
}

/*! 
 * @fn ccp_postprocess(dw1000_dev_instance_t * inst)
 *
 * @brief This serves as a place holder for timescale processing and by default is creates json string for the event
 *
 * input parameters
 * @param inst - dw1000_dev_instance_t * 
 *
 * output parameters
 *
 * returns none 
 */
static void ccp_postprocess(struct os_event * ev){
    assert(ev != NULL);
    assert(ev->ev_arg != NULL);
    dw1000_dev_instance_t * inst = (dw1000_dev_instance_t *)ev->ev_arg;
    dw1000_ccp_instance_t * ccp = inst->ccp; 
    ccp_frame_t * previous_frame = ccp->frames[(ccp->idx-1)%ccp->nframes]; 
    ccp_frame_t * frame = ccp->frames[(ccp->idx)%ccp->nframes]; 

    printf("{\"utime\": %lu,\"ccp_rx\":[\"%llX\",\"%llX\"],\"seq_num\":%d}\n", 
        os_cputime_ticks_to_usecs(os_cputime_get32()),
        frame->reception_timestamp,
        (uint64_t)((uint64_t)(frame->reception_timestamp) - (uint64_t)(previous_frame->reception_timestamp)) & 0xFFFFFFFFF,
        frame->seq_num
    );
}

/*! 
 * @fn ccp_rx_complete_cb(dw1000_dev_instance_t * inst)
 *
 * @brief Precise timing is achieved using the reception_timestamp and tracking intervals along with  
 * the correction factor. For timescale processing, a postprocessing 
 * callback is placed in the eventq
 *
 * input parameters
 * @param inst - dw1000_dev_instance_t * 
 *
 * output parameters
 *
 * returns none 
 */
static void 
ccp_rx_complete_cb(dw1000_dev_instance_t * inst){
    dw1000_ccp_instance_t * ccp = inst->ccp; 
    ccp_frame_t * frame = ccp->frames[(++ccp->idx)%ccp->nframes];

    ccp->status.valid |= ccp->idx > 1;
    if (ccp->status.valid){
        dw1000_read_rx(inst, frame->array, 0, sizeof(ieee_blink_frame_t));
        frame->reception_timestamp = dw1000_read_rxtime(inst); 
        int32_t tracking_interval = (int32_t) dw1000_read_reg(inst, RX_TTCKI_ID, 0, sizeof(int32_t));
        int32_t tracking_offset = (int32_t) dw1000_read_reg(inst, RX_TTCKO_ID, 0, sizeof(int32_t)) & RX_TTCKO_RXTOFS_MASK;
        frame->correction_factor = 1.0f + ((float)tracking_offset) / tracking_interval;
        if (ccp->config.postprocess) 
            os_eventq_put(os_eventq_dflt_get(), &ccp_callout_postprocess.c_ev);
    }
}


/*! 
 * @fn ccp_tx_complete_cb(dw1000_dev_instance_t * inst)
 *
 * @brief Precise timing is achieved by adding a fixed period to the transmission time of the previous frame. This static 
 * function is called on successful transmission of a CCP packet, and this advances the frame index point. Circular addressing is used 
 * for the frame addressing. The next os_event is scheduled to occur in (MYNEWT_VAL(CCP_PERIOD) - MYNEWT_VAL(CCP_OS_LATENCY)) usec 
 * from now. This provided a context switch guard zone. The assumption is that the underlying OS will have sufficient time start 
 * the call dw1000_set_delay_start within dw1000_ccp_blink. 
 *
 * input parameters
 * @param inst - dw1000_dev_instance_t * 
 *
 * output parameters
 *
 * returns none 
 */
static void 
ccp_tx_complete_cb(dw1000_dev_instance_t * inst){
    //Advance frame idx 
    dw1000_ccp_instance_t * ccp = inst->ccp; 
    ccp_frame_t * previous_frame = ccp->frames[(ccp->idx-1)%ccp->nframes]; 
    ccp_frame_t * frame = ccp->frames[(ccp->idx++)%ccp->nframes];
   

    printf("{\"utime\": %lu,\"ccp_tx\":[\"%llX\",\"%llX\"],\"seq_num\":%d}\n", 
        os_cputime_ticks_to_usecs(os_cputime_get32()),
        frame->transmission_timestamp,
        (uint64_t)((uint64_t)(frame->transmission_timestamp) - (uint64_t)(previous_frame->transmission_timestamp)) & 0xFFFFFFFFF,
        frame->seq_num
    );

    if (ccp->status.timer_enabled) 
        os_callout_reset(&ccp_callout_timer, OS_TICKS_PER_SEC * (ccp->period - MYNEWT_VAL(OS_LATENCY)) * 1e-6);    
    os_sem_release(&inst->ccp->sem);  
}


/*! 
 * @fn dw1000_ccp_blink(dw1000_dev_instance_t * inst, dw1000_ccp_modes_t mode)
 *
 * @brief Start clock calibration packets (CCP) blinks  with a pulse repetition period of MYNEWT_VAL(CCP_PERIOD). 
 * Precise timing is achieved by adding a fixed period to the transmission time of the previous frame. 
 * This removes the need to explicitly read the systime register and the assiciated non-deterministic latencies. 
 * This function is static function for internl use. It will force a Half Period Delay Warning is called at 
 * out of sequence.   
 *
 * input parameters
 * @param inst - dw1000_dev_instance_t * 
 * @param mode - dw1000_ccp_modes_t for CCP_BLOCKING, CCP_NONBLOCKING modes.
 *
 * output parameters
 *
 * returns dw1000_ccp_status_t 
 */
static dw1000_ccp_status_t 
dw1000_ccp_blink(dw1000_dev_instance_t * inst, dw1000_dev_modes_t mode){

    os_error_t err = os_sem_pend(&inst->ccp->sem,  OS_TIMEOUT_NEVER);
    assert(err == OS_OK);

    dw1000_ccp_instance_t * ccp = inst->ccp; 
    ccp_frame_t * frame = ccp->frames[(ccp->idx)%ccp->nframes];
    ccp_frame_t * previous_frame = ccp->frames[(ccp->idx-1)%ccp->nframes];

    frame->transmission_timestamp = previous_frame->transmission_timestamp + 2 * ((uint64_t)inst->ccp->period << 15);
    frame->seq_num+=inst->ccp->nframes;
    frame->long_address = inst->my_short_address;

    dw1000_write_tx(inst, frame->array, 0, sizeof(ieee_blink_frame_t));
    dw1000_write_tx_fctrl(inst, sizeof(ieee_blink_frame_t), 0, true); 
    dw1000_set_wait4resp(inst, false);    
    dw1000_set_delay_start(inst, frame->transmission_timestamp); 

    ccp->status.start_tx_error = dw1000_start_tx(inst).start_tx_error;
    if (ccp->status.start_tx_error){
        // Half Period Delay Warning occured try for the next epoch
        // Use seq_num to detect this on receiver size
        previous_frame->transmission_timestamp += ((uint64_t)inst->ccp->period << 15);
        os_sem_release(&inst->ccp->sem);
    }  
    else if(mode == DWT_BLOCKING){
        err = os_sem_pend(&inst->ccp->sem, OS_TIMEOUT_NEVER); // Wait for completion of transactions 
        os_sem_release(&inst->ccp->sem);
    }
   return ccp->status;
}


/*! 
 * @fn dw1000_ccp_start(dw1000_dev_instance_t * inst)
 *
 * @brief Start clock calibration packets (CCP) blinks. 
 * With a pulse repetition period of MYNEWT_VAL(CCP_PERIOD).   
 *
 * input parameters
 * @param dw1000_dev_instance_t * 
 *
 * output parameters
 *
 * no return value
 */
void 
dw1000_ccp_start(dw1000_dev_instance_t * inst){
    // Initialise frame timestamp to current time
    dw1000_ccp_instance_t * ccp = inst->ccp; 
    ccp->idx = 0x0;  
    ccp->status.valid = false;
    ccp_frame_t * frame = ccp->frames[(ccp->idx)%ccp->nframes]; 
    frame->transmission_timestamp = dw1000_read_systime(inst);
    ccp_timer_init(inst);
}

/*! 
 * @fn dw1000_ccp_stop(dw1000_dev_instance_t * inst)
 *
 * @brief Stop clock calibration packets (CCP) blinks.   
 *
 * input parameters
 * @param dw1000_dev_instance_t * 
 *
 * output parameters
 *
 * no return value
 */
void 
dw1000_ccp_stop(dw1000_dev_instance_t * inst){
    os_callout_stop(&ccp_callout_timer);
}

#endif /* MYNEWT_VAL(DW1000_CLOCK_CALIBRATION) */