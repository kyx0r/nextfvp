#define FFS_AUDIO	0x1000
#define FFS_VIDEO	0x2000
#define FFS_SUBTS	0x4000
#define FFS_STRIDX	0x0fff
#define FFS_SAMPLEFMT		AV_SAMPLE_FMT_S16
//#define FFS_CHLAYOUT		AV_CHANNEL_LAYOUT_STEREO

/* ffmpeg stream */
struct ffs {
	AVCodecContext *cc;
	AVFormatContext *fc;
	AVStream *st;
	AVPacket pkt;
	int si;			/* stream index */
	long ts;		/* frame timestamp (ms) */
	long pts;		/* last decoded packet pts in milliseconds */
	long dur;		/* last decoded packet duration */

	/* decoding video frames */
	struct SwsContext *swsc;
	struct SwrContext *swrc;
	AVFrame *dst;
	AVFrame *tmp;
};

static int ffs_stype(int flags)
{
	if (flags & FFS_VIDEO)
		return AVMEDIA_TYPE_VIDEO;
	if (flags & FFS_AUDIO)
		return AVMEDIA_TYPE_AUDIO;
	if (flags & FFS_SUBTS)
		return AVMEDIA_TYPE_SUBTITLE;
	return 0;
}

void ffs_free(struct ffs *ffs)
{
	if (ffs->swrc)
		swr_free(&ffs->swrc);
	if (ffs->swsc)
		sws_freeContext(ffs->swsc);
	if (ffs->dst)
		av_free(ffs->dst);
	if (ffs->tmp)
		av_free(ffs->tmp);
	if (ffs->cc)
		avcodec_free_context(&ffs->cc);
	if (ffs->fc)
		avformat_close_input(&ffs->fc);
	free(ffs);
}

struct ffs *ffs_alloc(char *path, int flags)
{
	struct ffs *ffs;
	int idx = (flags & FFS_STRIDX) - 1;
	ffs = malloc(sizeof(*ffs));
	memset(ffs, 0, sizeof(*ffs));
	ffs->si = -1;
	if (avformat_open_input(&ffs->fc, path, NULL, NULL))
		goto failed;
	if (avformat_find_stream_info(ffs->fc, NULL) < 0)
		goto failed;
	ffs->si = av_find_best_stream(ffs->fc, ffs_stype(flags), idx, -1, NULL, 0);
	if (ffs->si < 0)
		goto failed;
	ffs->st = ffs->fc->streams[ffs->si];
	const AVCodec *dec = avcodec_find_decoder(ffs->st->codecpar->codec_id);
	ffs->cc = avcodec_alloc_context3(dec);
	avcodec_parameters_to_context(ffs->cc, ffs->st->codecpar);
	if (avcodec_open2(ffs->cc, dec, NULL))
		goto failed;
	ffs->tmp = av_frame_alloc();
	ffs->dst = av_frame_alloc();
	return ffs;
failed:
	ffs_free(ffs);
	return NULL;
}

static AVPacket *ffs_pkt(struct ffs *ffs)
{
	AVPacket *pkt = &ffs->pkt;
	while (av_read_frame(ffs->fc, pkt) >= 0) {
		if (pkt->stream_index == ffs->si) {
			long pts = (pkt->dts == AV_NOPTS_VALUE ? 0 : pkt->dts) *
				av_q2d(ffs->st->time_base) * 1000;
			ffs->dur = MIN(MAX(0, pts - ffs->pts), 1000);
			if (pts > ffs->pts || pts + 200 < ffs->pts)
				ffs->pts = pts;
			return pkt;
		}
		av_packet_unref(pkt);
	}
	return NULL;
}

static long ts_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int wait(long ts, int vdelay)
{
	long nts = ts_ms();
	if (nts > ts && ts + vdelay > nts) {
		struct timespec req;
		req.tv_sec = 0;
		req.tv_nsec = (ts + vdelay - nts) * 1000000;
		struct timespec rem;
		nanosleep(&req, &rem);
		return 0;
	}
	return 1;
}

void ffs_wait(struct ffs *ffs)
{
	int vdelay = ffs->dur;
	if (!wait(ffs->ts, MAX(vdelay, 20)))
		ffs->ts += MAX(vdelay, 20);
	else
		ffs->ts = ts_ms();		/* out of sync */
}

/* audio/video frame offset difference */
int ffs_avdiff(struct ffs *ffs, struct ffs *affs)
{
	return affs->pts - ffs->pts;
}

long ffs_pos(struct ffs *ffs)
{
	return ffs->pts;
}

void ffs_seek(struct ffs *ffs, struct ffs *vffs, long pos)
{
	av_seek_frame(ffs->fc, vffs->si,
		pos / av_q2d(vffs->st->time_base) / 1000, 0);
	ffs->ts = 0;
}

void ffs_vinfo(struct ffs *ffs, int *w, int *h)
{
	*h = ffs->cc->height;
	*w = ffs->cc->width;
}

int ffs_vdec(struct ffs *ffs, void **buf)
{
	AVCodecContext *vcc = ffs->cc;
	AVPacket *pkt = ffs_pkt(ffs);
	if (!pkt)
		return -1;
	if (avcodec_send_packet(vcc, pkt) < 0)
		return 0;
	if (avcodec_receive_frame(vcc, ffs->tmp) < 0)
		return 0;
	av_packet_unref(pkt);
	if (buf) {
		sws_scale(ffs->swsc, (void *) ffs->tmp->data, ffs->tmp->linesize,
			  0, vcc->height, ffs->dst->data, ffs->dst->linesize);
		*buf = (void *) ffs->dst->data[0];
		return ffs->dst->linesize[0];
	}
	return 0;
}

int ffs_sdec(struct ffs *ffs, char *buf, int blen, long *beg, long *end)
{
	AVPacket *pkt = ffs_pkt(ffs);
	AVSubtitle sub = {0};
	AVSubtitleRect *rect;
	int fine = 0;
	int i;
	if (!pkt)
		return -1;
	avcodec_decode_subtitle2(ffs->cc, &sub, &fine, pkt);
	av_packet_unref(pkt);
	buf[0] = '\0';
	if (!fine)
		return 1;
	rect = sub.num_rects ? sub.rects[0] : NULL;
	if (rect && rect->text)
		snprintf(buf, blen, "%s", sub.rects[0]->text);
	if (rect && !rect->text && rect->ass) {
		char *s = rect->ass;
		for (i = 0; s && i < 9; i++)
			s = strchr(s, ',') ? strchr(s, ',') + 1 : NULL;
		if (s)
			snprintf(buf, blen, "%s", s);
	}
	if (strchr(buf, '\n'))
		*strchr(buf, '\n') = '\0';
	*beg = ffs->pts + sub.start_display_time * av_q2d(ffs->st->time_base) * 1000;
	*end = ffs->pts + sub.end_display_time * av_q2d(ffs->st->time_base) * 1000;
	avsubtitle_free(&sub);
	return 0;
}

static int ffs_bytespersample(struct ffs *ffs)
{
	return av_get_bytes_per_sample(FFS_SAMPLEFMT) * ffs->cc->ch_layout.nb_channels;
}

int ffs_adec(struct ffs *ffs, char *buf, int blen)
{
	int rdec = 0;
	AVPacket tmppkt = {0};
	AVPacket *pkt = ffs_pkt(ffs);
	uint8_t *out[] = {NULL};
	if (!pkt)
		return -1;
	tmppkt.size = pkt->size;
	tmppkt.data = pkt->data;
	if (avcodec_send_packet(ffs->cc, &tmppkt) < 0)
		return -1;
	while (tmppkt.size > 0) {
		if (avcodec_receive_frame(ffs->cc, ffs->tmp) < 0)
			break;
	        int len = av_get_bytes_per_sample(ffs->cc->sample_fmt);
		tmppkt.size -= len;
		tmppkt.data += len;
		out[0] = (uint8_t*)buf + rdec;
		len = swr_convert(ffs->swrc,
			out, (blen - rdec) / ffs_bytespersample(ffs),
			(void *) ffs->tmp->extended_data, ffs->tmp->nb_samples);
		if (len > 0)
			rdec += len * ffs_bytespersample(ffs);
	}
	av_packet_unref(pkt);
	return rdec;
}

static int fbm2pixfmt(int fbm)
{
	switch (fbm & 0x0fff) {
	case 0x888:
		return AV_PIX_FMT_RGB32;
	case 0x565:
		return AV_PIX_FMT_RGB565;
	case 0x233:
		return AV_PIX_FMT_RGB8;
	default:
		fprintf(stderr, "ffs: unknown fb_mode()\n");
		return AV_PIX_FMT_RGB32;
	}
}

void ffs_vconf(struct ffs *ffs, float zoom, int fbm)
{
	int h = ffs->cc->height;
	int w = ffs->cc->width;
	int fmt = ffs->cc->pix_fmt;
	int pixfmt = fbm2pixfmt(fbm);
	uint8_t *buf = NULL;
	int n;
	ffs->swsc = sws_getContext(w, h, fmt, w * zoom, h * zoom,
			pixfmt, SWS_FAST_BILINEAR,
			NULL, NULL, NULL);
	n = av_image_get_buffer_size(pixfmt, w * zoom, h * zoom, 16);
	buf = av_malloc(n * sizeof(uint8_t));
	av_image_fill_arrays(ffs->dst->data, ffs->dst->linesize, buf, pixfmt,
				w * zoom, h * zoom, 1);
}

void ffs_aconf(struct ffs *ffs)
{
	int ret = swr_alloc_set_opts2(&ffs->swrc,
		&ffs->cc->ch_layout, FFS_SAMPLEFMT, ffs->cc->sample_rate,
		&ffs->cc->ch_layout, ffs->cc->sample_fmt, ffs->cc->sample_rate,
		0, NULL);
	if (ret < 0) {
		fprintf(stderr, "ffs: swr_alloc_set_opts2 error\n");
		return;
	}
	swr_init(ffs->swrc);
}

void ffs_globinit(void)
{
	avformat_network_init();
}

long ffs_duration(struct ffs *ffs)
{
	if (ffs->st->duration != AV_NOPTS_VALUE)
		return ffs->st->duration * av_q2d(ffs->st->time_base) * 1000;
	if (ffs->fc->duration > 0)
		return ffs->fc->duration / (AV_TIME_BASE / 1000);
	return 0;
}
