/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "obs-internal.h"

void handle_encoder_group_reconfigure_request(obs_encoder_t *encoder);
extern void full_stop(struct obs_encoder *encoder);

static void stop_failed_encoder(void *param)
{
	obs_encoder_t *encoder = param;
	full_stop(encoder);
	obs_encoder_release(encoder);
}

/* ------------------------------------------------------------------------- */
/* Vulkan: frames as OS handles (docs/graphics-interop.md, "Texture encoders") */

/* Encoders may hold frames for their whole pipeline (NVENC: up to 64 with lookahead), so the pool grows instead of
 * waiting for them; past this it duplicates the previous frame, as when every frame is queued. */
#define MAX_SHARED_ENCODE_FRAMES 80

static enum gs_handle_type native_handle_type(void)
{
#ifdef _WIN32
	return GS_HANDLE_WIN32;
#else
	return GS_HANDLE_FD;
#endif
}

bool gpu_encode_create_textures(struct obs_core_video_mix *video, gs_texture_t **tex, gs_texture_t **tex_uv,
				uint32_t width, uint32_t height, bool p010)
{
	UNUSED_PARAMETER(video);
	*tex = *tex_uv = NULL;
	/* Vulkan has no D3D11 share handles; its encoder frames are exportable instead. */
	if (gs_get_device_type() == GS_DEVICE_VULKAN &&
	    gs_texture_create_exportable(width, height, GS_UNKNOWN, p010 ? GS_SHARED_P010 : GS_SHARED_NV12,
					 GS_RENDER_TARGET, tex, tex_uv))
		return true;
	if (p010)
		return gs_texture_create_p010(tex, tex_uv, width, height, GS_RENDER_TARGET | GS_SHARED_KM_TEX);
	return gs_texture_create_nv12(tex, tex_uv, width, height, GS_RENDER_TARGET | GS_SHARED_KM_TEX);
}

void gpu_encode_release_unref(struct obs_gpu_release *release)
{
	if (release && --release->refs == 0) {
		gs_shared_handle_close(&release->handle);
		gs_timeline_destroy(release->timeline);
		bfree(release);
	}
}

struct obs_gpu_release *gpu_encode_release_create(void)
{
	struct obs_gpu_release *release = bzalloc(sizeof(*release));
	release->refs = 1;
	release->timeline = gs_timeline_create(0);
	if (!release->timeline || !gs_timeline_export(release->timeline, native_handle_type(), &release->handle)) {
		blog(LOG_ERROR, "GPU encode: cannot create an exportable release timeline");
		gs_timeline_destroy(release->timeline);
		bfree(release);
		return NULL;
	}
	return release;
}

static bool gpu_encode_add_frame(struct obs_core_video_mix *video)
{
	const struct video_output_info *info = video_output_get_info(video->video);
	struct obs_tex_frame frame = {0};

	if (!gpu_encode_create_textures(video, &frame.tex, &frame.tex_uv, info->width, info->height,
					info->format == VIDEO_FORMAT_P010) ||
	    !frame.tex)
		return false;

#ifdef _WIN32
	frame.handle = gs_texture_get_shared_handle(frame.tex);
#else
	frame.handle = (uint32_t)-1;
#endif
	deque_push_back(&video->gpu_encoder_avail_queue, &frame, sizeof(frame));
	video->gpu_encode_frames++;
	return true;
}

/* Drops the holds that are over; true when nothing reads the frame any more. */
static bool gpu_frame_free(struct obs_tex_frame *tf)
{
	size_t kept = 0;
	for (size_t i = 0; i < tf->holds.num; i++) {
		struct obs_gpu_hold hold = tf->holds.array[i];
		if (os_atomic_load_bool(&hold.release->stopped) ||
		    gs_timeline_get_value(hold.release->timeline) >= hold.value)
			gpu_encode_release_unref(hold.release);
		else
			tf->holds.array[kept++] = hold;
	}
	tf->holds.num = kept;
	return kept == 0;
}

static void gpu_frame_free_resources(struct obs_tex_frame *tf)
{
	gs_texture_destroy(tf->tex);
	gs_texture_destroy(tf->tex_uv);
	if (tf->buf) {
		struct gs_shared_handle h = {tf->buf_desc.type, tf->buf_desc.win32, tf->buf_desc.fds[0]};
		gs_shared_handle_close(&h);
		gs_sharedbuf_destroy(tf->buf);
	}
	for (size_t i = 0; i < tf->holds.num; i++)
		gpu_encode_release_unref(tf->holds.array[i].release);
	da_free(tf->holds);
}

bool gpu_encode_pop_free_frame(struct obs_core_video_mix *video, struct obs_tex_frame *tf)
{
	const size_t count = video->gpu_encoder_avail_queue.size / sizeof(*tf);

	if (!video->gpu_encode_shared) {
		if (!count)
			return false;
		deque_pop_front(&video->gpu_encoder_avail_queue, tf, sizeof(*tf));
		return true;
	}

	for (size_t i = 0; i < count; i++) {
		deque_pop_front(&video->gpu_encoder_avail_queue, tf, sizeof(*tf));
		if (gpu_frame_free(tf))
			return true;
		deque_push_back(&video->gpu_encoder_avail_queue, tf, sizeof(*tf));
	}

	if (video->gpu_encode_frames < MAX_SHARED_ENCODE_FRAMES && gpu_encode_add_frame(video)) {
		deque_pop_back(&video->gpu_encoder_avail_queue, tf, sizeof(*tf));
		return true;
	}
	return false;
}

static void gpu_encode_export(struct obs_core_video_mix *video, gs_texture_t *tex, struct gs_shared_texture_desc *desc,
			      uint64_t *id)
{
	for (size_t i = 0; i < video->gpu_encode_exports.num; i++) {
		struct obs_gpu_export *e = &video->gpu_encode_exports.array[i];
		if (e->tex == tex) {
			*desc = e->desc;
			*id = e->id;
			return;
		}
	}

	struct obs_gpu_export e = {.tex = tex};
	if (!gs_texture_export(tex, native_handle_type(), &e.desc)) {
		memset(desc, 0, sizeof(*desc));
		*id = 0;
		return;
	}
	e.id = ++video->gpu_encode_next_id;
	da_push_back(video->gpu_encode_exports, &e);
	*desc = e.desc;
	*id = e.id;
}

void gpu_encode_prepare_frame(struct obs_core_video_mix *video, struct obs_tex_frame *tf)
{
	if (video->gpu_encode_buffer_users > 0) {
		if (!tf->buf) {
			const struct video_output_info *info = video_output_get_info(video->video);
			tf->buf = gs_sharedbuf_create(info->width, info->height,
						      info->format == VIDEO_FORMAT_P010 ? GS_SHARED_P010
											: GS_SHARED_NV12);
			if (tf->buf && gs_sharedbuf_export(tf->buf, native_handle_type(), &tf->buf_desc)) {
				tf->buf_id = ++video->gpu_encode_next_id;
			} else {
				blog(LOG_ERROR, "GPU encode: cannot create an exportable frame buffer");
				gs_sharedbuf_destroy(tf->buf);
				tf->buf = NULL;
			}
		}
		if (tf->buf)
			gs_sharedbuf_copy_texture(tf->buf, tf->tex);
	}

	gpu_encode_export(video, tf->tex, &tf->tex_desc, &tf->tex_id);

	tf->ready_value = ++video->gpu_encode_ready_value;
	gs_timeline_signal(video->gpu_encode_ready, tf->ready_value);
}

/* ------------------------------------------------------------------------- */

#define NBSP "\xC2\xA0"
static const char *gpu_encode_frame_name = "gpu_encode_frame";
static void *gpu_encode_thread(void *data)
{
	struct obs_core_video_mix *video = data;
	uint64_t interval = video_output_get_frame_time(video->video);
	DARRAY(obs_encoder_t *) encoders;
	int wait_frames = NUM_ENCODE_TEXTURE_FRAMES_TO_WAIT;

	da_init(encoders);

	os_set_thread_name("obs gpu encode thread");
	const char *gpu_encode_thread_name = profile_store_name(
		obs_get_profiler_name_store(), "obs_gpu_encode_thread(%g" NBSP "ms)", interval / 1000000.);
	profile_register_root(gpu_encode_thread_name, interval);

	while (os_sem_wait(video->gpu_encode_semaphore) == 0) {
		struct obs_tex_frame tf;
		uint64_t timestamp;
		uint64_t lock_key;
		uint64_t next_key;
		size_t lock_count = 0;
		uint64_t fer_ts = 0;

		if (os_atomic_load_bool(&video->gpu_encode_stop))
			break;

		if (wait_frames) {
			wait_frames--;
			continue;
		}

		profile_start(gpu_encode_thread_name);

		os_event_reset(video->gpu_encode_inactive);

		/* -------------- */

		pthread_mutex_lock(&video->gpu_encoder_mutex);

		deque_pop_front(&video->gpu_encoder_queue, &tf, sizeof(tf));
		const size_t tf_holds_before = tf.holds.num;
		timestamp = tf.timestamp;
		lock_key = tf.lock_key;
		next_key = tf.lock_key;

		video_output_inc_texture_frames(video->video);

		for (size_t i = 0; i < video->gpu_encoders.num; i++) {
			obs_encoder_t *encoder = obs_encoder_get_ref(video->gpu_encoders.array[i]);
			if (encoder)
				da_push_back(encoders, &encoder);
		}

		pthread_mutex_unlock(&video->gpu_encoder_mutex);

		/* -------------- */

		for (size_t i = 0; i < encoders.num; i++) {
			struct encoder_packet pkt = {0};
			bool received = false;
			bool success = false;
			uint32_t skip = 0;

			obs_encoder_t *encoder = encoders.array[i];
			obs_weak_encoder_t **paired = encoder->paired_encoders.array;
			size_t num_paired = encoder->paired_encoders.num;

			/* stopping, see below */
			if (os_atomic_load_bool(&encoder->gpu_encode_failed))
				continue;

			pkt.timebase_num = encoder->timebase_num * encoder->frame_rate_divisor;
			pkt.timebase_den = encoder->timebase_den;
			pkt.encoder = encoder;

			if (encoder->encoder_group && !encoder->start_ts) {
				struct obs_encoder_group *group = encoder->encoder_group;
				bool ready = false;
				pthread_mutex_lock(&group->mutex);
				ready = group->start_timestamp == timestamp;
				pthread_mutex_unlock(&group->mutex);
				if (!ready)
					continue;
			}

			if (!encoder->first_received && num_paired) {
				bool wait_for_audio = false;

				for (size_t idx = 0; !wait_for_audio && idx < num_paired; idx++) {
					obs_encoder_t *enc = obs_weak_encoder_get_encoder(paired[idx]);
					if (!enc)
						continue;

					if (!enc->first_received || enc->first_raw_ts > timestamp) {
						wait_for_audio = true;
					}

					obs_encoder_release(enc);
				}

				if (wait_for_audio)
					continue;
			}

			if (video_pause_check(&encoder->pause, timestamp))
				continue;

			// an explicit counter is used instead of remainder calculation
			// to allow multiple encoders started at the same time to start on
			// the same frame
			skip = encoder->frame_rate_divisor_counter++;
			if (encoder->frame_rate_divisor_counter == encoder->frame_rate_divisor)
				encoder->frame_rate_divisor_counter = 0;
			if (skip)
				continue;

			handle_encoder_group_reconfigure_request(encoder);

			if (encoder->reconfigure_requested) {
				encoder->reconfigure_requested = false;
				encoder->info.update(encoder->context.data, encoder->context.settings);
			}

			if (!encoder->start_ts)
				encoder->start_ts = timestamp;

			if (++lock_count == encoders.num)
				next_key = 0;
			else
				next_key++;

			/* Get the frame encode request timestamp. This
			 * needs to be read just before the encode request.
			 */
			fer_ts = os_gettime_ns();

			/* stop_gpu_encode clears gpu_release only once this thread is idle */
			struct obs_gpu_release *release = encoder->gpu_release;
			struct encoder_texture_shared shared = {0};
			if (release) {
				shared.tex_id = tf.tex_id;
				shared.buf_id = tf.buf ? tf.buf_id : 0;
				shared.tex = tf.tex_desc;
				if (tf.buf)
					shared.buf = tf.buf_desc;
				shared.ready = video->gpu_encode_ready_handle;
				shared.ready_value = tf.ready_value;
				shared.release = release->handle;
				shared.release_value = tf.ready_value;
			}

			profile_start(gpu_encode_frame_name);
			if (encoder->info.encode_texture2) {
				struct encoder_texture tex = {0};

				tex.handle = tf.handle;
				tex.tex[0] = tf.tex;
				tex.tex[1] = tf.tex_uv;
				tex.tex[2] = NULL;
				tex.shared = release ? &shared : NULL;
				success = encoder->info.encode_texture2(encoder->context.data, &tex, encoder->cur_pts,
									lock_key, &next_key, &pkt, &received);
				/* the encoder may read the frame until it signals its release timeline */
				if (release) {
					struct obs_gpu_hold hold = {release, tf.ready_value};
					da_push_back(tf.holds, &hold);
				}
			} else {
				success = encoder->info.encode_texture(encoder->context.data, tf.handle,
								       encoder->cur_pts, lock_key, &next_key, &pkt,
								       &received);
			}
			profile_end(gpu_encode_frame_name);

			/* Generate and enqueue the frame timing metrics, namely
			 * the CTS (composition time), FER (frame encode request), FERC
			 * (frame encode request complete) and current PTS. PTS is used to
			 * associate the frame timing data with the encode packet. */
			if (tf.timestamp) {
				struct encoder_packet_time *ept = da_push_back_new(encoder->encoder_packet_times);
				// Get the frame encode request complete timestamp
				if (success) {
					ept->ferc = os_gettime_ns();
				} else {
					// Encode had error, set ferc to 0
					ept->ferc = 0;
				}

				ept->pts = encoder->cur_pts;
				ept->cts = tf.timestamp;
				ept->fer = fer_ts;
			}

			if (success) {
				send_off_encoder_packet(encoder, success, received, &pkt);
			} else {
				/* full_stop() waits for this thread to go idle and may join it, so it runs on the
				 * destroy task thread instead of here */
				blog(LOG_ERROR, "Error encoding with encoder '%s'", encoder->context.name);
				os_atomic_set_bool(&encoder->gpu_encode_failed, true);
				obs_encoder_t *ref = obs_encoder_get_ref(encoder);
				if (ref)
					obs_queue_task(OBS_TASK_DESTROY, stop_failed_encoder, ref, false);
			}

			lock_key = next_key;

			encoder->cur_pts += encoder->timebase_num * encoder->frame_rate_divisor;
		}

		/* -------------- */

		pthread_mutex_lock(&video->gpu_encoder_mutex);

		/* references of the holds added above (stop_gpu_encode waits for this thread before letting go) */
		for (size_t i = tf_holds_before; i < tf.holds.num; i++)
			tf.holds.array[i].release->refs++;

		tf.lock_key = next_key;

		if (--tf.count) {
			tf.timestamp += interval;
			deque_push_front(&video->gpu_encoder_queue, &tf, sizeof(tf));

			video_output_inc_texture_skipped_frames(video->video);
		} else {
			deque_push_back(&video->gpu_encoder_avail_queue, &tf, sizeof(tf));
		}

		pthread_mutex_unlock(&video->gpu_encoder_mutex);

		/* -------------- */

		os_event_signal(video->gpu_encode_inactive);

		for (size_t i = 0; i < encoders.num; i++)
			obs_encoder_release(encoders.array[i]);

		da_resize(encoders, 0);

		profile_end(gpu_encode_thread_name);
		profile_reenable_thread();
	}

	da_free(encoders);
	return NULL;
}

bool init_gpu_encoding(struct obs_core_video_mix *video)
{
	video->gpu_encode_stop = false;

	video->gpu_encode_frames = 0;
	video->gpu_encode_buffer_users = 0;
	video->gpu_encode_ready_value = 0;

	/* Vulkan hands frames to encoders as OS handles, ordered by a timeline */
	video->gpu_encode_shared = false;
	if (gs_get_device_type() == GS_DEVICE_VULKAN) {
		video->gpu_encode_ready = gs_timeline_create(0);
		if (video->gpu_encode_ready && gs_timeline_export(video->gpu_encode_ready, native_handle_type(),
								  &video->gpu_encode_ready_handle)) {
			video->gpu_encode_shared = true;
		} else {
			blog(LOG_WARNING, "GPU encode: no exportable timeline, encoders get no shared frames");
			gs_timeline_destroy(video->gpu_encode_ready);
			video->gpu_encode_ready = NULL;
		}
	}

	deque_reserve(&video->gpu_encoder_avail_queue, NUM_ENCODE_TEXTURES * sizeof(struct obs_tex_frame));
	for (size_t i = 0; i < NUM_ENCODE_TEXTURES; i++) {
		if (!gpu_encode_add_frame(video))
			return false;
	}

	if (os_sem_init(&video->gpu_encode_semaphore, 0) != 0)
		return false;
	if (os_event_init(&video->gpu_encode_inactive, OS_EVENT_TYPE_MANUAL) != 0)
		return false;
	if (pthread_create(&video->gpu_encode_thread, NULL, gpu_encode_thread, video) != 0)
		return false;

	os_event_signal(video->gpu_encode_inactive);

	video->gpu_encode_thread_initialized = true;
	return true;
}

void stop_gpu_encoding_thread(struct obs_core_video_mix *video)
{
	if (video->gpu_encode_thread_initialized) {
		os_atomic_set_bool(&video->gpu_encode_stop, true);
		os_sem_post(video->gpu_encode_semaphore);
		pthread_join(video->gpu_encode_thread, NULL);
		video->gpu_encode_thread_initialized = false;
	}
}

void free_gpu_encoding(struct obs_core_video_mix *video)
{
	if (video->gpu_encode_semaphore) {
		os_sem_destroy(video->gpu_encode_semaphore);
		video->gpu_encode_semaphore = NULL;
	}
	if (video->gpu_encode_inactive) {
		os_event_destroy(video->gpu_encode_inactive);
		video->gpu_encode_inactive = NULL;
	}

#define free_deque(x)                                               \
	do {                                                        \
		while (x.size) {                                    \
			struct obs_tex_frame frame;                 \
			deque_pop_front(&x, &frame, sizeof(frame)); \
			gpu_frame_free_resources(&frame);           \
		}                                                   \
		deque_free(&x);                                     \
	} while (false)

	free_deque(video->gpu_encoder_queue);
	free_deque(video->gpu_encoder_avail_queue);
#undef free_deque

	for (size_t i = 0; i < video->gpu_encode_exports.num; i++) {
		struct gs_shared_texture_desc *d = &video->gpu_encode_exports.array[i].desc;
		struct gs_shared_handle h = {d->type, d->win32, d->fds[0]};
		gs_shared_handle_close(&h);
	}
	da_free(video->gpu_encode_exports);
	gs_shared_handle_close(&video->gpu_encode_ready_handle);
	gs_timeline_destroy(video->gpu_encode_ready);
	video->gpu_encode_ready = NULL;
	video->gpu_encode_shared = false;
	video->gpu_encode_frames = 0;
}
