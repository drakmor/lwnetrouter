// router_hpiaudio.cpp
//
// Audio router using an AudioScience HPI device.
//
//   (C) Copyright 2016 Fred Gleason <fredg@paravelsystems.com>
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as
//   published by the Free Software Foundation; either version 2 of
//   the License, or (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public
//   License along with this program; if not, write to the Free Software
//   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//

#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "router_hpiaudio.h"

size_t SetpointBytes(uint32_t msec)
{
  return AUDIO_SAMPLE_RATE*AUDIO_CHANNELS*msec*sizeof(float)/1000;
}


hpi_err_t HpiError(hpi_err_t err,const QString &flag="")
{
  char str[200];
  if(err!=0) {
    HPI_GetErrorText(err,str);
    if(flag.isEmpty()) {
      syslog(LOG_INFO,"HPI error %s",str);
    }
    else {
      syslog(LOG_INFO,"[%s] HPI error %s",(const char *)flag.toUtf8(),str);
    }
  }
  return err;
}


void *__AudioCallback(void *ptr)
{
  static RouterHpiAudio *rha=(RouterHpiAudio *)ptr;
  static uint16_t in_state;
  static uint32_t in_buffer_size;
  static uint32_t in_data_len;
  static uint32_t in_frame_len;
  static uint32_t in_aux_len;
  static uint16_t out_state;
  static uint32_t out_buffer_size;
  static uint32_t out_data_to_play;
  static uint32_t out_frames_played;
  static uint32_t out_aux_to_play;
  static float pcm[262144];
  static int inputs=rha->config()->inputQuantity();
  //  static int outputs=rha->config()->outputQuantity();
  static Ringbuffer *rb[MAX_INPUTS];
  static float timescale;
  //  static int xpoints[MAX_OUTPUTS];
  static int i;
  static uint32_t space;
  static uint32_t delay_frames;
  static uint32_t delay_interval;

  //
  // Initialize Input Queues
  //
  for(int i=0;i<inputs;i++) {
    rb[i]=
      new Ringbuffer(SetpointBytes(MAX_DELAY*1100));
    rha->delay_state_taken[i]=Config::DelayBypassed;
  }

  //
  // Main Loop
  //
  while(1==1) {
    //
    // Get Crosspoint Table
    //
    /*
    for(i=0;i<outputs;i++) {
      xpoints[i]=rha->crossPoint(i);
    }
    */

    //
    // Process Inputs
    //
    in_data_len=0;
    HpiError(HPI_InStreamGetInfoEx(NULL,rha->hpi_input_streams[0],&in_state,
				   &in_buffer_size,&in_data_len,&in_frame_len,
				   &in_aux_len));
    for(i=0;i<inputs;i++) {
      HpiError(HPI_InStreamReadBuf(NULL,rha->hpi_input_streams[i],
				   (uint8_t *)pcm,in_data_len));
      rb[i]->write(pcm,in_data_len);
    }
 
    //
    // Process Outputs
    //
    if(in_data_len>0) {
      for(i=0;i<inputs;i++) {
	//
	// Process Dump
	//
	if(rha->delay_dump[i]) {
	  switch(rha->delay_state_taken[i]) {
	  case Config::DelayEntering:
	  case Config::DelayEntered:
	    rb[i]->dump();
	    rha->delay_interval[i]=0;
	    rha->delay_dump[i]=false;
	    rha->delay_state_taken[i]=Config::DelayEntering;
	    break;

	  case Config::DelayExiting:
	    rb[i]->dump();
	    rha->delay_interval[i]=0;
	    rha->delay_dump[i]=false;
	    rha->delay_state_taken[i]=Config::DelayExited;
	    break;
	  case Config::DelayUnknown:
	  case Config::DelayBypassed:
	  case Config::DelayExited:
	    break;
	  }
	}

	HpiError(HPI_OutStreamGetInfoEx(NULL,rha->hpi_output_streams[i],
					&out_state,
					&out_buffer_size,&out_data_to_play,
					&out_frames_played,&out_aux_to_play));
	space=38400-out_data_to_play;
	space=rb[i]->read((float *)pcm,space);
	delay_frames=out_data_to_play+rb[i]->readSpace();

	switch(rha->delay_state_set[i]) {
	case Config::DelayEntering:
	  if(delay_frames>SetpointBytes(MAX_DELAY*1000)) {
	    timescale=1.0;
	    rha->delay_state_taken[i]=Config::DelayEntered;
	    rha->delay_interval[i]=MAX_DELAY*1000;
	  }
	  else {
	    if(rha->delay_state_taken[i]!=Config::DelayEntered) {
	      if((delay_interval=100*(delay_frames/38400))>
		 rha->delay_interval[i]) {
		rha->delay_interval[i]=delay_interval;
	      }
	      timescale=rha->delay_change_up;   // SLOWER
	      rha->delay_state_taken[i]=Config::DelayEntering;
	    }
	  }
	  break;

	case Config::DelayExiting:
	  if(delay_frames<4096) {
	    timescale=1.0;
	    rha->delay_state_taken[i]=Config::DelayExited;
	    rha->delay_interval[i]=0;
	  }
	  else {
	    if(rha->delay_state_taken[i]!=Config::DelayExited) {
	      if((delay_interval=100*(delay_frames/38400))<
		 rha->delay_interval[i]) {
		rha->delay_interval[i]=delay_interval;
	      }
	      timescale=rha->delay_change_down;   // FASTER
	      rha->delay_state_taken[i]=Config::DelayExiting;
	    }
	  }
	  break;

	case Config::DelayUnknown:
	case Config::DelayBypassed:
	case Config::DelayEntered:
	case Config::DelayExited:
	  timescale=1.0;
	  break;
	}

	HpiError(HPI_OutStreamSetTimeScale(NULL,rha->hpi_output_streams[i],
				       timescale*HPI_OSTREAM_TIMESCALE_UNITS));
	HpiError(HPI_OutStreamWriteBuf(NULL,rha->hpi_output_streams[i],
				       (uint8_t *)pcm,
				       space,rha->hpi_format),"OUTPUT");
	if(out_state==HPI_STATE_DRAINED) {
	  syslog(LOG_WARNING,"Xrun on output stream %d",i);
	}
	if(out_state==HPI_STATE_STOPPED) {
	  HpiError(HPI_OutStreamStart(NULL,rha->hpi_output_streams[i]));
	}
      }
    }
    usleep(AUDIO_HPI_POLLING_INTERVAL);
  }

  return NULL;
}




RouterHpiAudio::RouterHpiAudio(Config *c,QObject *parent)
  : Router(c,parent)
{
  hpi_format=new struct hpi_format;
  short on_gain[HPI_MAX_CHANNELS]={0,0};
  short off_gain[HPI_MAX_CHANNELS]={-10000,-10000};
  uint16_t ostreams;
  uint16_t istreams;
  uint16_t version;
  uint32_t serial;
  uint16_t type;
  uint32_t bufsize;

  //
  // Delay Settings
  //
  delay_change_down=1.0-(float)(config()->audioDelayChangePercent())/100;
  delay_change_up=1.0+(float)(config()->audioDelayChangePercent())/100;

  //
  // Get Adapter Info
  //
  HpiError(HPI_AdapterGetInfo(NULL,0,&ostreams,&istreams,&version,&serial,
			      &type));
  HpiError(HPI_FormatCreate(hpi_format,2,HPI_FORMAT_PCM32_FLOAT,
			    AUDIO_SAMPLE_RATE,0,0));
  HpiError(HPI_StreamEstimateBufferSize(hpi_format,AUDIO_HPI_POLLING_INTERVAL,
					&bufsize));

  //
  // Initialize Mixer
  //
  HpiError(HPI_MixerOpen(NULL,0,&hpi_mixer));
  for(int i=0;i<ostreams;i++) {
    for(int j=0;j<config()->outputQuantity();j++) {
      HpiError(HPI_MixerGetControl(NULL,hpi_mixer,
				   HPI_SOURCENODE_OSTREAM,i,
				   HPI_DESTNODE_LINEOUT,j,
				   HPI_CONTROL_VOLUME,
				   &hpi_output_volumes[i][j]));
      HpiError(HPI_VolumeSetGain(NULL,hpi_output_volumes[i][j],off_gain));
    }
  }
  for(int i=0;i<config()->outputQuantity();i++) {
    HpiError(HPI_VolumeSetGain(NULL,hpi_output_volumes[i][i],on_gain));
  }

  //
  // Open Inputs
  //
  for(int i=0;i<config()->inputQuantity();i++) {
    HpiError(HPI_InStreamOpen(NULL,0,i,&hpi_input_streams[i]));
    HpiError(HPI_InStreamSetFormat(NULL,hpi_input_streams[i],hpi_format));
    if(config()->audioInputBusXfers()) {
      if(HpiError(HPI_InStreamHostBufferAllocate(NULL,hpi_input_streams[i],
						 bufsize))==
	 HPI_ERROR_INVALID_DATASIZE) {
	syslog(LOG_DEBUG,
	       "unable to enable bus mastering for input stream %d",i);
      }
    }
    HpiError(HPI_InStreamStart(NULL,hpi_input_streams[i]));
  }

  //
  // Open Outputs
  //
  uint8_t pcm[38400];
  memset(pcm,0,38400);
  for(int i=0;i<config()->inputQuantity();i++) {
    delay_state_set[i]=Config::DelayBypassed;
    delay_dump[i]=false;
    HpiError(HPI_OutStreamOpen(NULL,0,i,&hpi_output_streams[i]));
    HpiError(HPI_OutStreamSetFormat(NULL,hpi_output_streams[i],hpi_format));
    HpiError(HPI_OutStreamSetTimeScale(NULL,hpi_output_streams[i],
				       HPI_OSTREAM_TIMESCALE_PASSTHROUGH));
    if(config()->audioOutputBusXfers()) {
      if(HpiError(HPI_OutStreamHostBufferAllocate(NULL,hpi_output_streams[i],
						  bufsize))==
	 HPI_ERROR_INVALID_DATASIZE) {
	syslog(LOG_DEBUG,
	       "unable to enable bus mastering for output stream %d",i);
      }
    }
    HpiError(HPI_OutStreamWriteBuf(NULL,hpi_output_streams[i],pcm,38400,
				   hpi_format));
  }

  //
  // Timers
  //
  hpi_scan_timer=new QTimer(this);
  connect(hpi_scan_timer,SIGNAL(timeout()),this,SLOT(scanTimerData()));
  hpi_scan_timer->start(100);

  //
  // Start the Callback
  //
  pthread_attr_t pthread_attr;
  pthread_attr_init(&pthread_attr);
  pthread_attr_setschedpolicy(&pthread_attr,SCHED_FIFO);
  pthread_create(&hpi_pthread,&pthread_attr,__AudioCallback,this);
}


Config::DelayState RouterHpiAudio::delayState(int input) const
{
  return Config::DelayBypassed;
}


int RouterHpiAudio::delayInterval(int input)
{
  return 0;
}


void RouterHpiAudio::setDelayState(int input,Config::DelayState state)
{
  printf("setDelayState(%d,%d)\n",input,state);

  switch(state) {
  case Config::DelayEntering:
    delay_state_set[input]=Config::DelayEntering;
    break;

  case Config::DelayExiting:
    delay_state_set[input]=Config::DelayExiting;
    break;

  case Config::DelayUnknown:
  case Config::DelayBypassed:
  case Config::DelayEntered:
  case Config::DelayExited:
    break;
  }
}


void RouterHpiAudio::dumpDelay(int input)
{
  delay_dump[input]=true;
  printf("dumpDelay(%d)\n",input);
}


void RouterHpiAudio::scanTimerData()
{
  for(int i=0;i<config()->inputQuantity();i++) {
    if((hpi_delay_state[i]!=delay_state_taken[i])||
       (hpi_delay_interval[i]!=delay_interval[i])) {
      hpi_delay_state[i]=delay_state_taken[i];
      hpi_delay_interval[i]=delay_interval[i];
      emit delayStateChanged(i,hpi_delay_state[i],hpi_delay_interval[i]);
    }
  }
}