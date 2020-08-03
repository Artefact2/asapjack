/* Author: Romain "Artefact2" Dal Maso <artefact2@gmail.com>
 *
 * This program is free software. It comes without any warranty, to the
 * extent permitted by applicable law. You can redistribute it and/or
 * modify it under the terms of the Do What The Fuck You Want To Public
 * License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details. */

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <jack/jack.h>
#include <samplerate.h>
#include <asap.h>

static SRC_STATE* src_ctx;
static ASAP* asap_ctx;
static jack_client_t* jack_ctx;
static jack_port_t* jack_left;
static jack_port_t* jack_right;
static float* buffer_left;
static float* buffer_right;

static jack_status_t st;
static int error;

static unsigned char asap_buf[4096]; /* XXX: what is an appropriate size? */
static float asap_buf2[4096];

static void* moddata;
static int modfd, moddata_length;

static long gen_samples(void* payload, float** data) {
	ASAP_Generate(asap_ctx, asap_buf, sizeof(asap_buf), ASAPSampleFormat_U8); /* XXX: check return value? */

	for(size_t i = 0; i < sizeof(asap_buf); ++i) {
		asap_buf2[i] = (float)(asap_buf[i] - 128) / 128.f;
	}

	*data = asap_buf2;
	return sizeof(asap_buf) / 2; /* number of generated frames */
}

static int jack_process(jack_nframes_t nframes, void* payload) {
	float buf[2*nframes]; /* XXX need to deinterleave the frames, this sucks */

	buffer_left = jack_port_get_buffer(jack_left, nframes);
	buffer_right = jack_port_get_buffer(jack_right, nframes);

	assert(src_callback_read(src_ctx, (double)jack_get_sample_rate(jack_ctx) / ASAP_SAMPLE_RATE, nframes, buf) == nframes);
	for(size_t i = 0; i < nframes; ++i) {
		buffer_left[i] = buf[i*2];
		buffer_right[i] = buf[i*2+1];
	}

	return 0;
}

int main(int argc, char** argv) {
	jack_ctx = jack_client_open("asapjack", JackNullOption, &st);
	assert(st == 0);
	jack_left = jack_port_register(jack_ctx, "left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
	assert(jack_left > 0);
	jack_right = jack_port_register(jack_ctx, "right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
	assert(jack_right > 0);

	modfd = open(argv[1], O_RDONLY);
	assert(modfd != -1);
	moddata_length = lseek(modfd, 0, SEEK_END);
	assert(moddata_length != -1);
	assert(lseek(modfd, 0, SEEK_SET) == 0);
	moddata = mmap(0, moddata_length, PROT_READ, MAP_SHARED, modfd, 0);
	assert(moddata != MAP_FAILED);
	close(modfd);

	asap_ctx = ASAP_New();
	error = ASAP_Load(asap_ctx, argv[1], moddata, moddata_length);
	assert(error == 1); /* XXX: ambiguous documentation */
	ASAP_PlaySong(asap_ctx, 0, -1);

	src_ctx = src_callback_new(gen_samples, 2, 2, &error, 0);
	assert(error == 0);

	error = jack_set_process_callback(jack_ctx, jack_process, 0);
	jack_activate(jack_ctx);
	error = jack_connect(jack_ctx, "asapjack:left", "system:playback_1");
	assert(error == 0);
	error = jack_connect(jack_ctx, "asapjack:right", "system:playback_2");
	assert(error == 0);

	while(1) {
		sleep(1); /* XXX: check end of song playback */
	}

	jack_deactivate(jack_ctx);

	src_delete(src_ctx);
	ASAP_Delete(asap_ctx);

	assert(munmap(moddata, moddata_length) == 0);

	assert(jack_port_unregister(jack_ctx, jack_left) == 0);
	assert(jack_port_unregister(jack_ctx, jack_right) == 0);
	assert(jack_client_close(jack_ctx) == 0);
	return 0;
}