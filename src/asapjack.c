/* Author: Romain "Artefact2" Dal Maso <artefact2@gmail.com>
 *
 * This program is free software. It comes without any warranty, to the
 * extent permitted by applicable law. You can redistribute it and/or
 * modify it under the terms of the Do What The Fuck You Want To Public
 * License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details. */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <asap.h>
#include <jack/jack.h>
#include <samplerate.h>

#define BUFSZ 128

struct playback_context {
	jack_client_t* jack_ctx;
	jack_port_t* left;
	jack_port_t* right;
	SRC_STATE* src_ctx;
	ASAP* asap_ctx;
	ASAPInfo const* info;

	pthread_mutex_t asap_mutex;
	int generated;
	int paused;
	int index; /* index of current module in argv */

	unsigned char asap_out[BUFSZ];
	float src_in[BUFSZ];
};



static long gen_samples_stereo(void* payload, float** data) {
	struct playback_context* p = payload;

	p->generated = ASAP_Generate(p->asap_ctx, p->asap_out, BUFSZ, ASAPSampleFormat_U8);
	memset(&(p->asap_out[p->generated]), 128, BUFSZ - p->generated);

	for(size_t i = 0; i < BUFSZ; ++i) {
		p->src_in[i] = (float)(p->asap_out[i] - 128) / 128.f;
	}

	*data = p->src_in;
	return BUFSZ / 2; /* number of generated frames */
}

static long gen_samples_mono(void* payload, float** data) {
	/* upmix to stereo */
	/* ASAP_Generate takes a given number of SAMPLES, not FRAMES */
	struct playback_context* p = payload;

	p->generated = ASAP_Generate(p->asap_ctx, p->asap_out, BUFSZ / 2, ASAPSampleFormat_U8);
	memset(&(p->asap_out[p->generated]), 128, BUFSZ / 2 - p->generated);

	for(size_t i = 0; i < BUFSZ / 2; ++i) {
		p->src_in[2*i+1] = p->src_in[2*i] = (float)(p->asap_out[i] - 128) / 128.f;
	}

	*data = p->src_in;
	return BUFSZ / 2;
}

static int jack_process(jack_nframes_t nframes, void* payload) {
	struct playback_context* p = payload; /* XXX there's got to be a better way */
	float buf[2*nframes]; /* XXX need to deinterleave the frames, this sucks */
	float* buffer_left = jack_port_get_buffer(p->left, nframes);
	float* buffer_right = jack_port_get_buffer(p->right, nframes);

	if(!(p->paused) && p->asap_ctx && pthread_mutex_trylock(&(p->asap_mutex)) == 0) {
		if(src_callback_read(p->src_ctx, (double)jack_get_sample_rate(p->jack_ctx) / ASAP_SAMPLE_RATE, nframes, buf) != nframes) {
			fprintf(stderr, "jack_process(): src_callback_read returned less than %d samples\n", nframes);
		}
		pthread_mutex_unlock(&(p->asap_mutex));

		for(size_t i = 0; i < nframes; ++i) {
			buffer_left[i] = buf[i*2];
			buffer_right[i] = buf[i*2+1];
		}
	} else {
		memset(buffer_left, 0, nframes * sizeof(float));
		memset(buffer_right, 0, nframes * sizeof(float));
	}

	return 0;
}

static void playback_path(const char* path, struct playback_context* p) {
	void* moddata = 0;
	int modfd = -1, moddata_length, error, millis, song, duration;
	fd_set s_in;
	struct timeval ts;
	int ret;

	modfd = open(path, O_RDONLY);
	if(modfd == -1) {
		perror("open()");
		goto cleanup; /* XXX? */
	}

	moddata_length = lseek(modfd, 0, SEEK_END);
	if(moddata_length == -1 || lseek(modfd, 0, SEEK_SET) != 0) {
		perror("lseek()");
		goto cleanup;
	}

	moddata = mmap(0, moddata_length, PROT_READ, MAP_SHARED, modfd, 0);
	if(moddata == MAP_FAILED) {
		perror("mmap()");
		goto cleanup;
	}

	pthread_mutex_lock(&(p->asap_mutex));
	p->asap_ctx = ASAP_New();
	if(ASAP_Load(p->asap_ctx, path, moddata, moddata_length) != 1) {
		pthread_mutex_unlock(&(p->asap_mutex));
		goto cleanup;
	}
	p->info = ASAP_GetInfo(p->asap_ctx);
	song = ASAPInfo_GetDefaultSong(p->info);
	duration = ASAPInfo_GetDuration(p->info, song);
	ASAP_PlaySong(p->asap_ctx, song, duration);
	if(duration == -1) {
		ASAP_DetectSilence(p->asap_ctx, 2); /* XXX */
	}
	p->src_ctx = src_callback_new(ASAPInfo_GetChannels(p->info) == 1 ? gen_samples_mono : gen_samples_stereo, 2, 2, &error, p);
	if(error != 0) {
		pthread_mutex_unlock(&(p->asap_mutex));
		goto cleanup;
	}
	p->generated = 1;
	millis = 0;
	pthread_mutex_unlock(&(p->asap_mutex));

	printf("==> %s by %s\n", ASAPInfo_GetTitleOrFilename(p->info), *ASAPInfo_GetAuthor(p->info) == 0 ? "unknown" : ASAPInfo_GetAuthor(p->info));

	while(p->generated > 0) {
		FD_ZERO(&s_in);
		FD_SET(STDIN_FILENO, &s_in);
		ts.tv_sec = 0;
		ts.tv_usec = 0;
		if((ret = select(1, &s_in, 0, 0, &ts)) == -1) {
			perror("select()");
			goto cleanup;
		}

		if(ret == 1) {
			char c = 0;
			if(read(STDIN_FILENO, &c, 1) < 1) {
				perror("read()");
			}

			switch(c) {
			case 'q':
				p->index = -1;
				putchar('\n');
				goto cleanup;

			case 'n':
				putchar('\n');
				goto cleanup;

			case 'p':
				p->index -= 2;
				putchar('\n');
				goto cleanup;

			case ' ':
				p->paused = !(p->paused);
				break;
			}
		}

		millis = ASAP_GetPosition(p->asap_ctx);
		printf("\r%c[2K%02d:%02d.%03d", 27, millis / 60000, (millis / 1000) % 60, millis % 1000);
		if(p->paused) fputs(" -- paused --", stdout);
		fflush(stdout);
		usleep(50000);
	}
	putchar('\n');

cleanup:

	pthread_mutex_lock(&(p->asap_mutex));
	if(p->src_ctx) {
		src_delete(p->src_ctx);
		p->src_ctx = 0;
	}

	if(p->asap_ctx) {
		ASAP_Delete(p->asap_ctx);
		p->asap_ctx = 0;
	}
	pthread_mutex_unlock(&(p->asap_mutex));

	if(modfd != -1) {
		if(close(modfd) == -1) {
			perror("close()");
		}
	}
	if(moddata != 0) {
		if(munmap(moddata, moddata_length) != 0) {
			perror("munmap()");
		}
	}
}

static void usage(const char* me) {
	printf(
		"Usage:\n\t%s [-h|--help]\n\t%s files...\n\n"
		"Interactive commands:\n\tq: quit the program\n\tspace: pause/resume playback\n\tp/n: jump to previous/next file\n\n"
		"ASAP version: %s\n%s"
		, me, me, ASAPInfo_VERSION, ASAPInfo_CREDITS
		);
}

int main(int argc, char** argv) {
	if(argc == 1 || strcmp("-h", argv[1]) == 0 || strcmp("--help", argv[1]) == 0) {
		usage(argv[0]);
		return 0;
	}

	struct termios termios_p_orig, termios_p;
	struct playback_context p;
	jack_status_t st;

	memset(&p, 0, sizeof(struct playback_context));
	termios_p.c_iflag = 0;

	if(tcgetattr(STDIN_FILENO, &termios_p_orig) != 0) {
		perror("tcgetattr()");
		goto cleanup;
	}

	termios_p = termios_p_orig;
	termios_p.c_lflag &= ~(ICANON | ECHO);
	if(tcsetattr(STDIN_FILENO, TCSANOW, &termios_p) != 0) {
		perror("tcsetattr()");
		goto cleanup;
	}

	if(pthread_mutex_init(&(p.asap_mutex), 0) != 0) {
		goto cleanup; /* XXX */
	}

	p.jack_ctx = jack_client_open("asapjack", JackNullOption, &st);
	if(st != 0) {
		goto cleanup;
	}

	p.left = jack_port_register(p.jack_ctx, "left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
	p.right = jack_port_register(p.jack_ctx, "right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
	if(p.left <= 0 || p.right <= 0) {
		goto cleanup;
	}

	if(jack_set_process_callback(p.jack_ctx, jack_process, &p) != 0 || jack_activate(p.jack_ctx) != 0) {
		goto cleanup;
	}

	if(jack_connect(p.jack_ctx, "asapjack:left", "system:playback_1") != 0 || jack_connect(p.jack_ctx, "asapjack:right", "system:playback_2") != 0) {
		goto cleanup;
	}

	p.index = 1;
	while(p.index >= 1 && p.index < argc) {
		playback_path(argv[p.index], &p);
		++p.index;
	}

cleanup:
	if(termios_p.c_iflag) {
		tcsetattr(STDIN_FILENO, TCSANOW, &termios_p_orig); /* XXX check failure? */
	}

	if(p.jack_ctx != 0) {
		/* XXX: handle failure in any of these calls */
		jack_deactivate(p.jack_ctx);

		if(p.left != 0) {
			jack_port_unregister(p.jack_ctx, p.left);
			p.left = 0;
		}
		if(p.right != 0) {
			jack_port_unregister(p.jack_ctx, p.right);
			p.right = 0;
		}

		jack_client_close(p.jack_ctx);
		p.jack_ctx = 0;
	}

	pthread_mutex_destroy(&(p.asap_mutex)); /* XXX: check if initialised? */
	return 0;
}
