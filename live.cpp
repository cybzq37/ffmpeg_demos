
#define SESSIONS 1
#define W 1920
#define H 1080
#define WSTR "1920"
#define HSTR "1080"
#define HW 1920
#define HH 1080
#define HWSTR "1920"
#define HHSTR "1080"

const int bitrate = 6000 * 1024;

// https://blog.howardlau.me/programming/libav-multi-stream-broadcasting.html
// 以C的方式引入头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cassert>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct QueueItem {
	AVFrame* frame;
	int is_audio;
	int pts;
	bool operator<(QueueItem const& other) const { return pts < other.pts; }
};

const int framerate = 60;
const int samplerate = 48000;
std::mutex mu;
std::deque<QueueItem> output_queue;
std::condition_variable cond;
int64_t start = av_gettime();
int64_t oframes = 0;

std::string outputURL;
std::string inputPrefix;
std::string texts[5] = { "设备测试", "参赛选手 1", "参赛选手 2", "参赛选手 3", "参赛选手 4" };
int64_t lastFrame[4] = { -1, -1, -1, -1 };



AVFrame* null_frame;
std::array<std::mutex, SESSIONS> locks;

std::array<AVFilterContext*, SESSIONS> inputs{};
std::array<AVFilterContext*, SESSIONS> scaled{};
std::array<AVFilterContext*, SESSIONS> compose_inputs{};
AVFilterContext* composed;

// 创建合成器滤镜图，用于将多个输入流组合成一个输出流
// 当前实现只使用了in0输入，将其pad到WxH分辨率后输出
void compositor() {
	int ret = 0;
	// 分配输入连接点数组，每个会话对应一个输入连接点
	AVFilterInOut* input_inouts[SESSIONS];
	// 分配输出连接点结构体，用于描述滤镜图的输出端点
	AVFilterInOut* output_inout = avfilter_inout_alloc();
	// 获取buffersink滤镜，用于从滤镜图输出端获取处理后的帧
	const AVFilter* buffersink = avfilter_get_by_name("buffersink");
	// 获取buffer滤镜，用于向滤镜图输入端推送原始帧
	const AVFilter* buffer = avfilter_get_by_name("buffer");
	// 输出滤镜上下文，用于从滤镜图获取处理后的帧
	AVFilterContext* output_filter_ctx = nullptr;
	// 分配滤镜图结构体，用于管理整个滤镜链
	AVFilterGraph* filter_graph = avfilter_graph_alloc();
	// 创建输出滤镜（buffersink），命名为"out"，用于从滤镜图获取处理后的帧
	ret = avfilter_graph_create_filter(&output_filter_ctx, buffersink, "out",
		NULL, NULL, filter_graph);
	if (ret) {
		printf("create out filter error %d\n", ret);
		avfilter_inout_free(&output_inout);
		avfilter_graph_free(&filter_graph);
		return;
	}
	// 为每个会话创建输入滤镜和连接点
	for (int i = 0; i < SESSIONS; ++i) {
		// 初始化输入滤镜上下文指针
		compose_inputs[i] = nullptr;
		// 生成输入滤镜名称，格式为"in0"、"in1"等
		std::string name = "in" + std::to_string(i);
		// 创建输入滤镜（buffer），命名为"in0"、"in1"等
		// 设置输入参数：视频尺寸为HWxHH（1920x1080），像素格式为0（YUV420P），时间基为1/60，像素宽高比为1:1
		ret =
			avfilter_graph_create_filter(&compose_inputs[i], buffer, name.c_str(),
				"video_size=" HWSTR "x" HHSTR
				":pix_fmt=0:time_base=1/60:pixel_aspect=1",
				NULL, filter_graph);
		if (ret) {
			printf("create in filter %d error %d\n", i, ret);
			// 清理已分配的输入连接点
			for (int j = 0; j < i; ++j) {
				avfilter_inout_free(&input_inouts[j]);
			}
			avfilter_inout_free(&output_inout);
			avfilter_graph_free(&filter_graph);
			return;
		}
		// 为当前会话分配输入连接点
		input_inouts[i] = avfilter_inout_alloc();
		// 初始化连接点的next指针为nullptr
		input_inouts[i]->next = nullptr;
		// 设置连接点的名称，与输入滤镜名称一致
		input_inouts[i]->name = av_strdup(name.c_str());
		// 将连接点关联到对应的输入滤镜上下文
		input_inouts[i]->filter_ctx = compose_inputs[i];
		// 设置使用第0个pad（输入端口）
		input_inouts[i]->pad_idx = 0;
		// 将当前连接点链接到前一个连接点，形成链表结构
		if (i > 0) {
			input_inouts[i - 1]->next = input_inouts[i];
		}
	}

	// 配置输出连接点：设置名称为"out"，关联到输出滤镜上下文，使用第0个pad
	output_inout->name = av_strdup("out");
	output_inout->filter_ctx = output_filter_ctx;
	output_inout->pad_idx = 0;
	output_inout->next = nullptr;

	// 设置输出像素格式列表：只接受YUV420P格式，以AV_PIX_FMT_NONE结尾
	std::array<AVPixelFormat, 2> pix_fmts = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
	// 为输出滤镜设置像素格式选项，确保输出为YUV420P格式
	av_opt_set_int_list(output_filter_ctx, "pix_fmts", pix_fmts.data(), AV_PIX_FMT_NONE,
		AV_OPT_SEARCH_CHILDREN);
	// 构建滤镜描述字符串：
	// [in0] - 使用第一个输入端点（in0）
	// setpts=PTS-STARTPTS - 重置时间戳，从0开始
	// pad=W:H - 将视频pad（填充/裁剪）到WxH分辨率（1920x1080）
	// [out] - 输出端点
	// 注意：当前只使用了in0，其他输入（in1, in2等）虽然创建了但未在滤镜链中使用
	std::string filter_desc = std::string() + "[in0]setpts=PTS-STARTPTS,pad=" WSTR ":" HSTR " [out]";
	// 解析滤镜描述字符串，构建滤镜图，连接输入和输出端点
	// 注意：avfilter_graph_parse_ptr 成功时会接管并释放 input_inouts 和 output_inout
	ret = avfilter_graph_parse_ptr(filter_graph, filter_desc.c_str(),
		&output_inout, input_inouts, nullptr);
	if (ret < 0) {
		printf("compositor graph parse failed %d\n", ret);
		// 失败时需要手动释放连接点
		for (int i = 0; i < SESSIONS; ++i) {
			avfilter_inout_free(&input_inouts[i]);
		}
		avfilter_inout_free(&output_inout);
		avfilter_graph_free(&filter_graph);
		return;
	}
	// 成功时连接点已被接管，标记为 nullptr 避免重复释放
	output_inout = nullptr;
	for (int i = 0; i < SESSIONS; ++i) {
		input_inouts[i] = nullptr;
	}
	// 配置滤镜图，验证并初始化所有滤镜连接
	ret = avfilter_graph_config(filter_graph, nullptr);
	if (ret < 0) {
		printf("compositor graph config failed %d\n", ret);
		avfilter_graph_free(&filter_graph);
		return;
	}
	// 将输出滤镜上下文保存到全局变量，供后续使用（用于获取合成后的帧）
	composed = output_filter_ctx;
}

// 创建视频缩放滤镜图，将输入视频从WxH分辨率缩放到HWxHH分辨率
// 参数i：会话索引，用于标识不同的输入流
void scaler(int i) {
	assert(i >= 0 && i < SESSIONS);
	int ret = 0;

	AVFilterInOut* input_inout = avfilter_inout_alloc();
	AVFilterInOut* output_inout = avfilter_inout_alloc();
	if (input_inout == nullptr || output_inout == nullptr) {
		std::cout << "alloc filter inout failed\n";
		if (input_inout != nullptr) avfilter_inout_free(&input_inout);
		if (output_inout != nullptr) avfilter_inout_free(&output_inout);
		return;
	}

	const AVFilter* buffersink = avfilter_get_by_name("buffersink"); // 输出滤镜
	const AVFilter* buffer = avfilter_get_by_name("buffer"); // 输入滤镜
	AVFilterContext* output_filter_ctx = nullptr; // 输出滤镜上下文
	AVFilterContext* input_filter_ctx = nullptr; // 输入滤镜上下文
	AVFilterGraph* filter_graph = avfilter_graph_alloc(); // 滤镜图
	if (filter_graph == nullptr) {
		std::cout << "alloc filter graph failed\n";
		avfilter_inout_free(&input_inout);
		avfilter_inout_free(&output_inout);
		return;
	}

	// 创建输出滤镜（buffersink），命名为"out"
	ret = avfilter_graph_create_filter(&output_filter_ctx, buffersink, "out",
		nullptr, nullptr, filter_graph);
	if (ret < 0) {
		std::cout << "create scaler out filter " << i << " error " << ret << '\n';
		avfilter_inout_free(&input_inout);
		avfilter_inout_free(&output_inout);
		avfilter_graph_free(&filter_graph);
		return;
	}
	// 创建输入滤镜（buffer），命名为"in"
	// 设置输入参数：视频尺寸为WxH（1920x1080），像素格式为0（YUV420P），时间基为1/60，像素宽高比为1:1
	ret = avfilter_graph_create_filter(&input_filter_ctx, buffer, "in",
		"video_size=" WSTR "x" HSTR ":pix_fmt=0:time_base=1/60:pixel_aspect=1", nullptr, filter_graph);
	if (ret < 0) {
		std::cout << "create scaler in filter " << i << " error " << ret << '\n';
		avfilter_inout_free(&input_inout);
		avfilter_inout_free(&output_inout);
		avfilter_graph_free(&filter_graph);
		return;
	}

	// 配置输出连接点：设置名称为"out"，关联到输出滤镜上下文，使用第0个pad
	output_inout->name = av_strdup("out");
	output_inout->filter_ctx = output_filter_ctx;
	output_inout->pad_idx = 0;
	output_inout->next = nullptr;

	// 配置输入连接点：设置名称为"in"，关联到输入滤镜上下文，使用第0个pad
	input_inout->name = av_strdup("in");
	input_inout->filter_ctx = input_filter_ctx;
	input_inout->pad_idx = 0;
	input_inout->next = nullptr;

	// 设置输出像素格式列表：只接受YUV420P格式，以AV_PIX_FMT_NONE结尾
	std::array<AVPixelFormat, 2> pix_fmts = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
	av_opt_set_int_list(output_filter_ctx, "pix_fmts", pix_fmts.data(),
		AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

	// 构建滤镜描述字符串：
	// [in] - 输入端点
	// setpts=PTS-STARTPTS - 重置时间戳，从0开始
	// scale=w=HW:h=HH - 缩放视频到HWxHH分辨率（1920x1080）
	// [out] - 输出端点
	std::string filter_desc = "[in]setpts=PTS-STARTPTS,scale=w=" HWSTR ":h=" HHSTR "[out]";
	// 解析滤镜描述字符串，构建滤镜图，连接输入和输出端点
	// 注意：avfilter_graph_parse 会接管并释放 input_inout 和 output_inout
	// avfilter_graph_parse 的参数顺序是：graph, filters, inputs, outputs, log_ctx
	ret = avfilter_graph_parse(filter_graph, filter_desc.c_str(), input_inout,
		output_inout, nullptr);
	input_inout = nullptr; // 已被 avfilter_graph_parse 接管
	output_inout = nullptr; // 已被 avfilter_graph_parse 接管
	if (ret < 0) {
		printf("graph parse failed %d\n", ret);
		avfilter_graph_free(&filter_graph);
		return;
	}
	// 配置滤镜图，验证并初始化所有滤镜连接
	ret = avfilter_graph_config(filter_graph, nullptr);
	if (ret < 0) {
		printf("graph config failed %d\n", ret);
		avfilter_graph_free(&filter_graph);
		return;
	}
	// 将输出滤镜上下文保存到全局数组，供后续使用（用于获取缩放后的帧）
	// 注意：filter_graph 需要保持存在，因为滤镜上下文依赖于它
	scaled.at(i) = output_filter_ctx;
	// 将输入滤镜上下文保存到全局数组，供后续使用（用于推送原始帧）
	inputs.at(i) = input_filter_ctx;
	// filter_graph 会在程序结束时或显式清理时释放
}

// 生成一个占位画面帧（null_frame），当输入流不可用或出错时作为默认画面使用
// 使用FFmpeg的testsrc滤镜生成彩色测试图案，解码后转换为YUV420P格式
void blank_screen_generator() {
	int ret = 0;

	// ========== 第一步：分配格式上下文 ==========
	AVFormatContext* ctx = avformat_alloc_context();
	if (ctx == nullptr) {
		printf("alloc format context failed\n");
		return;
	}

	// ========== 第二步：构建测试源URL ==========
	// testsrc是FFmpeg的测试源滤镜，生成彩色测试图案
	// size指定分辨率（1920x1080），rate指定帧率（60fps）
	std::string url = "testsrc=size=" HWSTR "x" HHSTR ":rate=60";

	// ========== 第三步：查找输入格式 ==========
	// lavfi是libavfilter输入格式，用于处理虚拟输入源（如testsrc）
	AVInputFormat* fmt = av_find_input_format("lavfi");
	if (fmt == nullptr) {
		printf("find lavfi format failed\n");
		avformat_free_context(ctx);
		return;
	}

	// ========== 第四步：打开输入流 ==========
	ret = avformat_open_input(&ctx, url.c_str(), fmt, NULL);
	if (ret) {
		printf("open null input %d\n", ret);
		// 注意：avformat_open_input失败时，ctx可能已被释放或设置为NULL
		if (ctx != nullptr) {
			avformat_free_context(ctx);
		}
		return;
	}

	// ========== 第五步：查找流信息 ==========
	ret = avformat_find_stream_info(ctx, NULL);
	if (ret < 0) {
		printf("find null stream info %d\n", ret);
		avformat_close_input(&ctx);
		return;
	}

	// ========== 第六步：查找视频流 ==========
	int vidx = -1;
	vidx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (vidx < 0) {
		printf("find_best_stream failed\n");
		avformat_close_input(&ctx);
		return;
	}

	// ========== 第七步：创建解码器 ==========
	AVCodec* decode_codec = avcodec_find_decoder(ctx->streams[vidx]->codecpar->codec_id);
	if (decode_codec == nullptr) {
		printf("find decoder failed\n");
		avformat_close_input(&ctx);
		return;
	}

	AVCodecContext* decode_ctx = avcodec_alloc_context3(decode_codec);
	if (decode_ctx == nullptr) {
		printf("alloc codec context failed\n");
		avformat_close_input(&ctx);
		return;
	}

	// 将流的编码参数复制到解码器上下文
	avcodec_parameters_to_context(decode_ctx, ctx->streams[vidx]->codecpar);

	// 打开解码器
	ret = avcodec_open2(decode_ctx, decode_codec, NULL);
	if (ret < 0) {
		printf("open codec failed %d\n", ret);
		avcodec_free_context(&decode_ctx);
		avformat_close_input(&ctx);
		return;
	}

	// ========== 第八步：打印调试信息 ==========
	printf("------ null ------\n");
	av_dump_format(ctx, 0, url.c_str(), 0);

	// ========== 第九步：分配数据包和帧 ==========
	AVPacket* packet = av_packet_alloc();
	if (packet == nullptr) {
		printf("alloc packet failed\n");
		avcodec_free_context(&decode_ctx);
		avformat_close_input(&ctx);
		return;
	}

	AVFrame* frame = av_frame_alloc();
	if (frame == nullptr) {
		printf("alloc frame failed\n");
		av_packet_free(&packet);
		avcodec_free_context(&decode_ctx);
		avformat_close_input(&ctx);
		return;
	}

	// ========== 第十步：读取和解码一帧 ==========
	while (true) {
		// 从输入流读取一个数据包
		ret = av_read_frame(ctx, packet);
		if (ret) {
			printf("read null frame error %d\n", ret);
			av_packet_free(&packet);
			av_frame_free(&frame);
			avcodec_free_context(&decode_ctx);
			avformat_close_input(&ctx);
			return;
		}

		// 只处理视频流的数据包
		if (packet->stream_index == vidx) {
			// 发送数据包给解码器
			ret = avcodec_send_packet(decode_ctx, packet);
			if (ret < 0) {
				printf("codec send packet error %d\n", ret);
				av_packet_unref(packet);
				av_packet_free(&packet);
				av_frame_free(&frame);
				avcodec_free_context(&decode_ctx);
				avformat_close_input(&ctx);
				return;
			}

			// 释放数据包的引用
			av_packet_unref(packet);

			// 从解码器接收解码后的帧
			ret = avcodec_receive_frame(decode_ctx, frame);
			if (ret < 0) {
				printf("codec receive frame error %d\n", ret);
				av_packet_free(&packet);
				av_frame_free(&frame);
				avcodec_free_context(&decode_ctx);
				avformat_close_input(&ctx);
				return;
			}

			// ========== 第十一步：分配并初始化null_frame ==========
			// 为全局变量null_frame分配内存
			null_frame = av_frame_alloc();
			if (null_frame == nullptr) {
				printf("alloc null_frame failed\n");
				av_packet_free(&packet);
				av_frame_free(&frame);
				avcodec_free_context(&decode_ctx);
				avformat_close_input(&ctx);
				return;
			}

			// 设置null_frame的基本属性
			null_frame->format = AV_PIX_FMT_YUV420P;  // YUV420P格式（最常用的视频格式）
			null_frame->width = frame->width;
			null_frame->height = frame->height;

			// ========== 第十二步：分配YUV420P缓冲区 ==========
			// 计算YUV420P格式所需的缓冲区大小（字节数）
			// 注意：av_image_get_buffer_size已经返回字节数，不需要再乘以sizeof(uint8_t)
			int buf_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frame->width,
				frame->height, 1);
			if (buf_size < 0) {
				printf("get buffer size failed %d\n", buf_size);
				av_frame_free(&null_frame);
				av_packet_free(&packet);
				av_frame_free(&frame);
				avcodec_free_context(&decode_ctx);
				avformat_close_input(&ctx);
				return;
			}

			// 分配缓冲区内存
			uint8_t* buf = (uint8_t*)av_malloc(buf_size);
			if (buf == nullptr) {
				printf("alloc buffer failed\n");
				av_frame_free(&null_frame);
				av_packet_free(&packet);
				av_frame_free(&frame);
				avcodec_free_context(&decode_ctx);
				avformat_close_input(&ctx);
				return;
			}

			// 填充null_frame的data和linesize数组
			// 设置Y、U、V三个平面的数据指针和每行的字节数
			ret = av_image_fill_arrays(null_frame->data, null_frame->linesize, buf,
				AV_PIX_FMT_YUV420P, frame->width, frame->height, 1);
			if (ret < 0) {
				printf("fill arrays failed %d\n", ret);
				av_free(buf);
				av_frame_free(&null_frame);
				av_packet_free(&packet);
				av_frame_free(&frame);
				avcodec_free_context(&decode_ctx);
				avformat_close_input(&ctx);
				return;
			}

			// ========== 第十三步：格式转换 ==========
			// 创建图像转换上下文（SwsContext）
			// testsrc输出RGB24格式，需要转换为YUV420P格式
			// 注意：需要根据实际的frame格式来确定源格式，这里假设是RGB24
			enum AVPixelFormat src_fmt = (enum AVPixelFormat)frame->format;
			struct SwsContext* conv_ctx = sws_getContext(
				frame->width, frame->height, src_fmt,  // 源格式（使用frame的实际格式）
				frame->width, frame->height, AV_PIX_FMT_YUV420P,  // 目标格式
				SWS_BILINEAR, nullptr, nullptr, nullptr);

			if (conv_ctx == nullptr) {
				printf("create sws context failed\n");
				av_free(buf);
				av_frame_free(&null_frame);
				av_packet_free(&packet);
				av_frame_free(&frame);
				avcodec_free_context(&decode_ctx);
				avformat_close_input(&ctx);
				return;
			}

			// 执行格式转换
			ret = sws_scale(conv_ctx,
				(const uint8_t* const*)frame->data, frame->linesize, 0, frame->height,
				null_frame->data, null_frame->linesize);

			// 释放转换上下文
			sws_freeContext(conv_ctx);

			if (ret < 0) {
				printf("sws_scale failed %d\n", ret);
				av_free(buf);
				av_frame_free(&null_frame);
				av_packet_free(&packet);
				av_frame_free(&frame);
				avcodec_free_context(&decode_ctx);
				avformat_close_input(&ctx);
				return;
			}

			// ========== 第十四步：清理临时资源 ==========
			// 释放临时frame和packet（null_frame会一直使用，不需要释放）
			av_frame_free(&frame);
			av_packet_free(&packet);

			// 释放解码器上下文
			avcodec_free_context(&decode_ctx);

			// 关闭输入流
			avformat_close_input(&ctx);

			// 成功生成占位帧，退出函数
			return;
		}

		// 如果不是视频流的数据包，释放引用继续读取
		av_packet_unref(packet);
	}

	// 如果循环结束仍未找到视频帧，清理资源
	av_packet_free(&packet);
	av_frame_free(&frame);
	avcodec_free_context(&decode_ctx);
	avformat_close_input(&ctx);
	printf("no video frame found\n");
}

void audio_input_handler() {
	std::string url = "rtmp://localhost/live/audio";
	int64_t audio_counter = 0;
	while (true) {
		AVFormatContext* ctx = avformat_alloc_context();
		int ret = avformat_open_input(&ctx, url.c_str(), NULL, NULL);
		if (ret) {
			printf("open_audio input %d\n", ret);
			break;
		}
		ret = avformat_find_stream_info(ctx, NULL);
		if (ret < 0) {
			printf("find_stream_info %d\n", ret);
			return;
		}
		int aidx = -1;
		aidx = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
		if (aidx < 0) {
			printf("find_best_stream audio\n");
			return;
		}
		AVCodec* decode_codec =
			avcodec_find_decoder(ctx->streams[aidx]->codecpar->codec_id);
		AVCodecContext* decode_ctx = avcodec_alloc_context3(decode_codec);
		avcodec_parameters_to_context(decode_ctx, ctx->streams[aidx]->codecpar);
		avcodec_open2(decode_ctx, decode_codec, nullptr);
		printf("------ audio ------\n");
		av_dump_format(ctx, 0, url.c_str(), 0);
		AVPacket* packet = av_packet_alloc();
		AVFrame* frame = av_frame_alloc();
		int64_t delta = 0;
		int missed = 0;
		int64_t first_frame = -1;
		while (true) {
			ret = av_read_frame(ctx, packet);
			if (ret) {
				printf("read audio frame error %d\n", ret);
				break;
			}
			if (packet->stream_index == aidx) {
				ret = avcodec_send_packet(decode_ctx, packet);
				if (ret < 0) {
					printf("codec send packet error %d\n", ret);
					break;
				}
				while (true) {
					ret = avcodec_receive_frame(decode_ctx, frame);
					if (ret != 0) {
						av_frame_unref(frame);
						break;
					}
					{
						std::lock_guard<std::mutex> lock(mu);
						if (output_queue.size() < 120)
							output_queue.push_back({ av_frame_clone(frame), 1 });
						cond.notify_all();
					}
					// printf("audio %d %d %d\n", frame->pts, frame->pkt_dts,
					// frame->nb_samples);
					if (ret < 0) {
						printf("audio add buffersrc failed %d\n", ret);
						break;
					}
				}
			}
			av_packet_unref(packet);
		}
		avcodec_close(decode_ctx);
		avformat_close_input(&ctx);
		av_packet_free(&packet);
		av_frame_free(&frame);
	}
}


// 超过3秒未检测到新数据，超时
static int interrupt_cb(void* ctx) {
	int64_t last = *(int64_t*)(ctx);
	if (last > 0 && av_gettime() - last >= 3 * 1000000) {
		printf("timeout\n");
		return -1;
	}
	return 0;
}


// 输入流处理函数：从 RTMP 流读取视频帧，解码后送入滤镜图进行处理
// 参数：
//   url: RTMP 流地址（如 "rtmp://localhost/live/contest-1"）
//   i:   流索引（0, 1, 2...），用于标识不同的输入流
void input_stream_handler(const std::string& url, int i) {

	// 外层循环：持续尝试连接和处理输入流
	// 如果连接断开或出错，会清理资源后重新连接
	while (true) {
		// ========== 第一步：分配和初始化格式上下文 ==========
		// 分配一个 AVFormatContext 结构体，用于管理输入流的格式信息
		AVFormatContext* ctx = avformat_alloc_context();
		if (ctx == nullptr) {
			printf("alloc context failed %d\n", i);
			return;
		}

		int frames = 0;  // 帧计数器，用于统计已处理的帧数

		// 设置中断回调函数，用于检测流是否超时
		// opaque: 传递给回调函数的用户数据（第 i 个流的最后帧时间戳地址）
		// callback: 回调函数指针，FFmpeg 会在 I/O 操作期间定期调用此函数
		// 如果 lastFrame[i] 超过 3 秒未更新，回调函数返回 -1，FFmpeg 会中断当前操作
		ctx->interrupt_callback.opaque = &lastFrame[i];
		ctx->interrupt_callback.callback = interrupt_cb;

		// ========== 第二步：打开输入流 ==========
		// 打开指定的 RTMP URL，读取流的基本信息（但不读取具体数据）
		int ret = avformat_open_input(&ctx, url.c_str(), nullptr, nullptr);
		if (ret) {
			printf("open_input %d %d\n", i, ret);
			avformat_free_context(ctx);  // 打开失败，释放上下文
			return;  // 如果无法打开输入流，退出函数（不再重试）
		}

		// ========== 第三步：查找流信息 ==========
		// 读取流的详细信息，包括编码格式、分辨率、帧率等
		// 这个操作可能需要读取一些数据包来确定流的参数
		ret = avformat_find_stream_info(ctx, NULL);
		if (ret < 0) {
			printf("find_stream_info %d %d\n", i, ret);
			avformat_close_input(&ctx);  // 查找失败，关闭输入流
			return;
		}

		// ========== 第四步：查找视频流 ==========
		// 在输入流中找到最佳的视频流（如果有多个视频流，选择最合适的）
		int vidx = -1;
		vidx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
		if (vidx < 0) {
			printf("find_best_stream %d\n", i);
			avformat_close_input(&ctx);  // 未找到视频流，关闭输入
			return;
		}

		// ========== 第五步：创建解码器 ==========
		// 根据视频流的编码格式（如 H.264）查找对应的解码器
		AVCodec* decode_codec = avcodec_find_decoder(ctx->streams[vidx]->codecpar->codec_id);
		if (decode_codec == nullptr) {
			printf("find decoder failed %d\n", i);
			avformat_close_input(&ctx);
			return;
		}

		// 分配解码器上下文，用于存储解码器的状态和参数
		AVCodecContext* decode_ctx = avcodec_alloc_context3(decode_codec);
		if (decode_ctx == nullptr) {
			printf("alloc codec context failed %d\n", i);
			avformat_close_input(&ctx);
			return;
		}

		// 将流的编码参数复制到解码器上下文中
		avcodec_parameters_to_context(decode_ctx, ctx->streams[vidx]->codecpar);

		// 打开解码器，初始化解码器
		ret = avcodec_open2(decode_ctx, decode_codec, nullptr);
		if (ret < 0) {
			printf("open codec failed %d %d\n", i, ret);
			avcodec_free_context(&decode_ctx);  // 打开失败，释放解码器上下文
			avformat_close_input(&ctx);
			return;
		}

		// ========== 第六步：打印流信息（调试用） ==========
		printf("------ %d ------\n", i);
		av_dump_format(ctx, 0, url.c_str(), 0);

		// ========== 第七步：分配数据包和帧缓冲区 ==========
		// AVPacket: 用于存储压缩后的数据包（从流中读取的原始数据）
		AVPacket* packet = av_packet_alloc();
		if (packet == nullptr) {
			printf("alloc packet failed %d\n", i);
			avcodec_free_context(&decode_ctx);
			avformat_close_input(&ctx);
			return;
		}

		// AVFrame: 用于存储解码后的原始视频帧（YUV 格式）
		AVFrame* frame = av_frame_alloc();
		if (frame == nullptr) {
			printf("alloc frame failed %d\n", i);
			av_packet_free(&packet);
			avcodec_free_context(&decode_ctx);
			avformat_close_input(&ctx);
			return;
		}

		// 用于帧率控制和丢帧检测的变量
		int64_t delta = 0;        // 时间差（未使用）
		int missed = 0;          // 丢失帧计数（未使用）
		int64_t first_frame = -1; // 第一帧的时间戳，用于计算帧率

		// ========== 第八步：主循环 - 读取、解码和处理帧 ==========
		while (true) {
			// 从输入流中读取一个压缩数据包
			ret = av_read_frame(ctx, packet);

			// 更新最后帧时间戳，用于超时检测
			// interrupt_cb 会检查这个时间戳，如果超过 3 秒未更新，会中断操作
			lastFrame[i] = av_gettime();

			if (ret) {
				// 读取失败（可能是流结束、网络断开或超时）
				printf("read frame error %d %d\n", i, ret);
				break;  // 跳出内层循环，进入清理和重连流程
			}

			// 只处理视频流的数据包（忽略音频等其他流）
			if (packet->stream_index == vidx) {
				// 将压缩数据包发送给解码器
				ret = avcodec_send_packet(decode_ctx, packet);
				if (ret < 0) {
					printf("codec send packet error %d %d\n", i, ret);
					break;
				}

				// 循环从解码器接收解码后的帧
				// 一个数据包可能解码出多帧（如 B 帧），所以需要循环接收
				while (true) {
					// 从解码器接收一帧解码后的视频帧
					ret = avcodec_receive_frame(decode_ctx, frame);
					if (ret != 0) {
						// ret != 0 表示没有更多帧可接收（需要发送新的数据包）
						av_frame_unref(frame);  // 释放帧引用
						break;  // 跳出接收循环，继续读取下一个数据包
					}

					++frames;  // 帧计数器加 1

					// 记录第一帧的时间戳，用于后续的帧率控制
					if (first_frame < 0) {
						first_frame = av_gettime();
					}
					// 帧率控制：如果处理速度跟不上，丢弃当前帧
					// 66666 微秒 ≈ 1/15 秒，即期望每帧处理时间不超过 15fps
					// 如果当前时间超过预期时间，说明处理太慢，丢弃帧并重新连接
					else if (av_gettime() > first_frame + frames * 66666) {
						printf("%d discard frame %d\n", i, frames);
						av_frame_unref(frame);
						av_packet_unref(packet);
						goto retry;  // 跳转到清理和重连流程
					}

					// 将解码后的帧添加到滤镜图的输入缓冲区
					// 使用互斥锁保护，因为可能有多个线程同时操作
					{
						std::lock_guard<std::mutex> guard(locks[i]);
						// inputs[i] 是第 i 个流的输入滤镜（buffer），用于接收原始帧
						ret = av_buffersrc_add_frame(inputs[i], frame);
					}
					if (ret < 0) {
						printf("input %d add buffersrc failed %d\n", i, ret);
						break;  // 添加失败，跳出循环
					}
				}
			}

			// 释放数据包的引用（不释放 packet 结构体本身，只是释放数据）
			av_packet_unref(packet);
		}

		// ========== 第九步：清理资源并准备重连 ==========
	retry:
		// 重置最后帧时间戳，表示流已断开
		lastFrame[i] = -1;
		first_frame = -1;

		// 向滤镜图输入一个空帧（占位帧），表示当前流不可用
		// 这样输出端可以继续工作，使用占位画面
		av_buffersrc_write_frame(inputs[i], null_frame);

		// 释放解码器上下文
		// 注意：使用 avcodec_free_context 而不是 avcodec_close
		// 因为 decode_ctx 是用 avcodec_alloc_context3 分配的，需要用对应的释放函数
		avcodec_free_context(&decode_ctx);

		// 关闭输入流并释放格式上下文
		avformat_close_input(&ctx);

		// 释放数据包和帧的缓冲区
		av_packet_free(&packet);
		av_frame_free(&frame);

		// 清理完成后，外层 while(true) 循环会继续，重新尝试连接
		// 这样可以实现自动重连功能
	}
}

// 输出线程：从多个输入流获取帧，合成后输出
// 主要功能：
//   1. 从每个缩放后的流中获取最新帧
//   2. 将所有帧送入合成器（compositor）
//   3. 从合成器获取最终合成帧
//   4. 将合成帧放入输出队列，供编码线程使用
//   5. 控制输出帧率（60fps）
void output_thread() {
	int ret = 0;

	// stream_frames: 存储每个输入流的最新帧
	// 初始化为占位帧（null_frame），如果某个流断开，会显示占位画面
	AVFrame* stream_frames[SESSIONS];

	// output_frame: 用于从合成器获取最终输出帧
	AVFrame* output_frame = av_frame_alloc();
	if (output_frame == nullptr) {
		printf("alloc output_frame failed\n");
		return;
	}

	// 初始化每个流的帧为占位帧
	for (int i = 0; i < SESSIONS; ++i) {
		stream_frames[i] = av_frame_clone(null_frame);
		if (stream_frames[i] == nullptr) {
			printf("clone null_frame failed %d\n", i);
			// 清理已分配的帧
			for (int j = 0; j < i; ++j) {
				av_frame_free(&stream_frames[j]);
			}
			av_frame_free(&output_frame);
			return;
		}
	}

	// 主循环：持续生成输出帧
	while (true) {
		// 记录循环开始时间，用于帧率控制
		int64_t last = av_gettime();

		// 计算当前帧的 PTS（Presentation Time Stamp，显示时间戳）
		// 注意：这里使用整数除法，PTS 的单位是帧数（时间基为 1/60）
		// oframes 是全局变量，表示已输出的帧数
		// framerate = 60，所以 pts = oframes / 60 表示秒数
		// 但由于时间基是 1/60，所以 pts 应该等于 oframes（帧数）
		// 这里可能有逻辑问题，应该是 pts = oframes
		int64_t pts = oframes;  // 修正：直接使用帧数作为 PTS

		// pull_frame: 临时帧，用于从缩放滤镜中拉取帧
		// 注意：应该在循环外分配，避免每次循环都重新分配
		AVFrame* pull_frame = av_frame_alloc();
		if (pull_frame == nullptr) {
			printf("alloc pull_frame failed\n");
			continue;  // 分配失败，跳过本次循环
		}

		// ========== 第一步：从每个输入流获取最新帧 ==========
		for (int i = 0; i < SESSIONS; ++i) {
			// 使用互斥锁保护，因为可能有其他线程在操作 scaled[i]
			{
				std::lock_guard<std::mutex> guard(locks[i]);
				// 从缩放滤镜的输出端获取一帧
				// scaled[i] 是第 i 个流的缩放滤镜输出（buffersink）
				ret = av_buffersink_get_frame(scaled[i], pull_frame);
			}

			if (ret >= 0) {
				// 成功获取到新帧，更新 stream_frames[i]
				// 释放旧的帧
				av_frame_free(&stream_frames[i]);
				// 克隆新帧（因为 pull_frame 会被重复使用）
				stream_frames[i] = av_frame_clone(pull_frame);
				if (stream_frames[i] == nullptr) {
					printf("clone frame failed %d\n", i);
					// 如果克隆失败，使用占位帧
					stream_frames[i] = av_frame_clone(null_frame);
				}
				// 释放 pull_frame 中的数据引用（不释放结构体本身）
				av_frame_unref(pull_frame);
			}
			// 如果 ret < 0，表示没有新帧可用，继续使用 stream_frames[i] 中的旧帧

			// 设置帧的 PTS（显示时间戳）
			// 所有输入流使用相同的 PTS，确保同步
			stream_frames[i]->pts = pts;

			// 将帧写入合成器的输入缓冲区
			// compose_inputs[i] 是第 i 个流的合成器输入（buffer）
			ret = av_buffersrc_write_frame(compose_inputs[i], stream_frames[i]);
			if (ret < 0) {
				printf("add buffersrc i %d ret %d\n", i, ret);
				// 写入失败不影响其他流，继续处理
			}
		}

		// 释放临时帧
		av_frame_free(&pull_frame);

		// ========== 第二步：从合成器获取最终输出帧 ==========
		// composed 是合成器的输出（buffersink），包含所有输入流合成后的帧
		ret = av_buffersink_get_frame(composed, output_frame);
		if (ret < 0) {
			// 没有可用的输出帧（可能是合成器还未准备好）
			av_frame_unref(output_frame);
			// 注意：pull_frame 已经在上面释放了，这里直接 continue
			// 继续下一次循环，等待合成器准备好
			continue;
		}

		// ========== 第三步：将输出帧放入队列 ==========
		// 使用互斥锁保护输出队列
		{
			std::lock_guard<std::mutex> lock(mu);
			// 如果队列未满（小于 120 帧，约 2 秒的缓冲），添加帧
			if (output_queue.size() < 120) {
				// 克隆帧并放入队列（因为 output_frame 会被重复使用）
				// 第二个参数 0 表示这是视频帧（1 表示音频帧）
				output_queue.push_back({ av_frame_clone(output_frame), 0 });
			}
			// 如果队列已满，丢弃当前帧（避免队列无限增长）

			// 释放 output_frame 中的数据引用
			av_frame_unref(output_frame);

			// 通知等待的线程（编码线程）有新帧可用
			cond.notify_all();
		}

		// ========== 第四步：帧率控制 ==========
		// 计算本次循环消耗的时间
		// 16666 微秒 = 1/60 秒，即期望每帧处理时间不超过 16.666ms（60fps）
		int sl = (16666 - (av_gettime() - last));
		if (sl < 0) {
			// 处理时间超过预期，无法维持 60fps
			printf("deadline missed\n");
			// 不睡眠，立即开始下一次循环
			continue;
		}
		// 睡眠剩余时间，确保以 60fps 的速率输出
		av_usleep(sl);
	}
}



int aframes = 0;

// 生成 AAC 音频的 DSI（Decoder Specific Information，解码器特定信息）数据
// DSI 是 AAC 音频流中用于描述音频格式的元数据，通常存储在 extradata 中
// 参数：
//   sampling_frequency_index: 采样率索引（0-12，由 get_sr_index 函数获取）
//   channel_configuration: 声道配置（1=单声道, 2=立体声, 5=5.1声道等）
//   dsi: 输出的 DSI 数据缓冲区（至少需要 2 字节）
// 返回值：无（通过 dsi 参数返回结果）
//
// DSI 格式说明（根据 ISO/IEC 14496-3 标准）：
//   dsi[0]: 高 5 位 = object_type（对象类型，2=AAC LC），低 3 位 = sampling_frequency_index 的高位
//   dsi[1]: 最高位 = sampling_frequency_index 的最低位，中间 4 位 = channel_configuration，低 3 位保留
void make_dsi(unsigned int sampling_frequency_index,
	unsigned int channel_configuration, unsigned char* dsi) {
	unsigned int object_type = 2;  // AAC LC (Low Complexity) 类型，最常用的 AAC 编码格式

	// 第一个字节：object_type 左移 3 位（占高 5 位），sampling_frequency_index 右移 1 位（占低 3 位）
	dsi[0] = (object_type << 3) | (sampling_frequency_index >> 1);

	// 第二个字节：sampling_frequency_index 的最低位左移 7 位（占最高位），
	//             channel_configuration 左移 3 位（占中间 4 位），低 3 位保留为 0
	dsi[1] = ((sampling_frequency_index & 1) << 7) | (channel_configuration << 3);
}

// 根据采样率返回对应的索引值（用于 AAC DSI 编码）
// 参数：
//   sampling_frequency: 采样率（Hz），如 48000、44100 等
// 返回值：采样率索引（0-12），如果采样率不在支持列表中，返回 0（默认 96000Hz）
//
// AAC 标准支持的采样率索引表：
//   0  = 96000 Hz
//   1  = 88200 Hz
//   2  = 64000 Hz
//   3  = 48000 Hz  （最常用）
//   4  = 44100 Hz  （CD 质量）
//   5  = 32000 Hz
//   6  = 24000 Hz
//   7  = 22050 Hz
//   8  = 16000 Hz
//   9  = 12000 Hz
//   10 = 11025 Hz
//   11 = 8000 Hz   （电话质量）
//   12 = 7350 Hz
int get_sr_index(unsigned int sampling_frequency) {
	switch (sampling_frequency) {
	case 96000:
		return 0;
	case 88200:
		return 1;
	case 64000:
		return 2;
	case 48000:
		return 3;  // 最常用的采样率
	case 44100:
		return 4;  // CD 质量采样率
	case 32000:
		return 5;
	case 24000:
		return 6;
	case 22050:
		return 7;
	case 16000:
		return 8;
	case 12000:
		return 9;
	case 11025:
		return 10;
	case 8000:
		return 11;  // 电话质量采样率
	case 7350:
		return 12;
	default:
		return 0;  // 未知采样率，默认返回 0（96000Hz）
	}
}

// 输出IO线程：编码视频/音频帧并推送到RTMP流，同时保存到本地文件
// 主要功能：
//   1. 创建两个输出流：RTMP推流（ctx）和本地文件保存（fileCtx）
//   2. 配置H.264视频编码器和AAC音频编码器
//   3. 从队列中获取帧，编码后写入流
//   4. 使用BSF（Bitstream Filter）处理AAC音频格式转换
void output_io_thread() {
	int ret = 0;
	std::string url = outputURL;

	// ========== 第一步：查找编码器 ==========
	// 查找H.264视频编码器
	AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!codec) {
		printf("H.264 codec not found\n");
		return;
	}

	// 查找AAC音频编码器
	AVCodec* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!audio_codec) {
		printf("AAC codec not found\n");
		return;
	}

	// ========== 第二步：创建输出上下文 ==========
	AVFormatContext* ctx = nullptr;      // RTMP推流上下文
	AVFormatContext* fileCtx = nullptr; // 本地文件保存上下文

	// 为RTMP推流分配输出上下文（FLV格式）
	ret = avformat_alloc_output_context2(&ctx, NULL, "flv", url.c_str());
	if (ret) {
		printf("alloc_output RTMP context failed %d\n", ret);
		return;
	}

	// 生成本地文件名：前缀_时间戳.flv
	std::string fn = inputPrefix + "_" + std::to_string(av_gettime()) + ".flv";
	// 为本地文件分配输出上下文（FLV格式）
	ret = avformat_alloc_output_context2(&fileCtx, NULL, "flv", fn.c_str());
	if (ret) {
		printf("alloc_output file context failed %d\n", ret);
		avformat_free_context(ctx);
		return;
	}

	// ========== 第三步：打开输出IO ==========
	// 打开RTMP推流的IO（网络连接）
	ret = avio_open2(&ctx->pb, url.c_str(), AVIO_FLAG_WRITE,
		&ctx->interrupt_callback, NULL);
	if (ret) {
		printf("avio_open RTMP failed %d\n", ret);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// 打开本地文件的IO
	ret = avio_open2(&fileCtx->pb, fn.c_str(), AVIO_FLAG_WRITE,
		&fileCtx->interrupt_callback, NULL);
	if (ret) {
		printf("avio_open file failed %d\n", ret);
		avio_closep(&ctx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}
	// ========== 第四步：创建流 ==========
	// 为RTMP推流创建视频流和音频流
	AVStream* stream = avformat_new_stream(ctx, nullptr);
	AVStream* audio = avformat_new_stream(ctx, nullptr);
	if (stream == nullptr || audio == nullptr) {
		printf("create streams failed\n");
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// 为本地文件创建视频流和音频流
	AVStream* fs = avformat_new_stream(fileCtx, nullptr);
	AVStream* afs = avformat_new_stream(fileCtx, nullptr);
	if (fs == nullptr || afs == nullptr) {
		printf("create file streams failed\n");
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// ========== 第五步：分配编码器上下文 ==========
	AVCodecContext* ocodec_ctx = avcodec_alloc_context3(codec);
	AVCodecContext* audio_ctx = avcodec_alloc_context3(audio_codec);
	if (ocodec_ctx == nullptr || audio_ctx == nullptr) {
		printf("alloc codec context failed\n");
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// 如果输出格式需要全局头（如FLV），设置编码器标志
	// 全局头包含编码器的配置信息，放在文件开头
	if (ctx->oformat->flags & AVFMT_GLOBALHEADER) {
		ocodec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		audio_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	// ========== 第六步：配置音频编码器 ==========
	audio_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
	audio_ctx->codec_id = AV_CODEC_ID_AAC;
	audio_ctx->sample_rate = samplerate;        // 48000 Hz
	audio_ctx->bit_rate = 192 * 1024;          // 192 kbps
	audio_ctx->sample_fmt = audio_codec->sample_fmts[0];  // 使用编码器支持的第一个采样格式
	audio_ctx->channels = 2;                   // 立体声
	audio_ctx->time_base = (AVRational){ 1, samplerate };  // 时间基：1/48000 秒
	audio_ctx->channel_layout = av_get_default_channel_layout(2);  // 立体声布局

	// ========== 第七步：配置视频编码器 ==========
	ocodec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
	ocodec_ctx->codec_id = AV_CODEC_ID_H264;
	ocodec_ctx->width = W;                      // 1920
	ocodec_ctx->height = H;                     // 1080
	ocodec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;   // YUV420P 像素格式
	ocodec_ctx->gop_size = 5;                   // GOP大小：每5帧一个关键帧（I帧）
	ocodec_ctx->time_base = AVRational{ 1, framerate };  // 时间基：1/60 秒
	ocodec_ctx->bit_rate = bitrate;             // 6000 kbps

	// 设置H.264编码选项：零延迟和超快预设（适合实时推流）
	AVDictionary* opts = NULL;
	av_dict_set(&opts, "tune", "zerolatency", 0);   // 零延迟调优
	av_dict_set(&opts, "preset", "ultrafast", 0);   // 超快预设（编码速度快，但压缩率较低）

	// 打开视频编码器
	ret = avcodec_open2(ocodec_ctx, codec, &opts);
	if (ret < 0) {
		printf("open video codec failed %d\n", ret);
		av_dict_free(&opts);
		avcodec_free_context(&ocodec_ctx);
		avcodec_free_context(&audio_ctx);
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// 打开音频编码器
	ret = avcodec_open2(audio_ctx, audio_codec, nullptr);
	if (ret < 0) {
		printf("open audio codec failed %d\n", ret);
		av_dict_free(&opts);
		avcodec_free_context(&ocodec_ctx);
		avcodec_free_context(&audio_ctx);
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	av_dict_free(&opts);  // 释放选项字典
	// ========== 第八步：将编码器参数复制到流 ==========
	// 将编码器的参数（分辨率、编码格式等）复制到流的 codecpar 中
	avcodec_parameters_from_context(stream->codecpar, ocodec_ctx);
	avcodec_parameters_from_context(audio->codecpar, audio_ctx);
	avcodec_parameters_from_context(fs->codecpar, ocodec_ctx);
	avcodec_parameters_from_context(afs->codecpar, audio_ctx);

	// ========== 第九步：创建AAC BSF（Bitstream Filter） ==========
	// BSF用于转换AAC音频格式：从ADTS转换为ASC
	// ADTS（Audio Data Transport Stream）：带同步头的AAC格式，适合文件存储
	// ASC（Audio Specific Config）：不带同步头的AAC格式，适合流媒体传输（如FLV）
	AVBSFContext* aacbsf = nullptr;
	ret = av_bsf_alloc(av_bsf_get_by_name("aac_adtstoasc"), &aacbsf);
	if (ret < 0 || aacbsf == nullptr) {
		printf("alloc AAC BSF failed %d\n", ret);
		avcodec_free_context(&ocodec_ctx);
		avcodec_free_context(&audio_ctx);
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// 设置BSF的输入参数
	avcodec_parameters_copy(aacbsf->par_in, audio->codecpar);

	// 初始化BSF
	ret = av_bsf_init(aacbsf);
	if (ret < 0) {
		printf("init AAC BSF failed %d\n", ret);
		av_bsf_free(&aacbsf);
		avcodec_free_context(&ocodec_ctx);
		avcodec_free_context(&audio_ctx);
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// 将BSF的输出参数复制回流的 codecpar（BSF可能会修改参数）
	avcodec_parameters_copy(audio->codecpar, aacbsf->par_out);
	avcodec_parameters_copy(afs->codecpar, aacbsf->par_out);

	// ========== 第十步：写入文件头 ==========
	printf("------- Output -------\n");
	av_dump_format(ctx, 0, url.c_str(), 1);  // 打印流信息（调试用）

	// 写入RTMP流的文件头（包含流的元数据）
	ret = avformat_write_header(ctx, NULL);
	if (ret) {
		printf("write_header RTMP failed %d\n", ret);
		av_bsf_free(&aacbsf);
		avcodec_free_context(&ocodec_ctx);
		avcodec_free_context(&audio_ctx);
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// 写入本地文件的文件头
	ret = avformat_write_header(fileCtx, NULL);
	if (ret) {
		printf("write_header file failed %d\n", ret);
		av_bsf_free(&aacbsf);
		avcodec_free_context(&ocodec_ctx);
		avcodec_free_context(&audio_ctx);
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	// ========== 第十一步：准备编码循环 ==========
	AVPacket* packet = av_packet_alloc();  // 用于存储编码后的数据包
	if (packet == nullptr) {
		printf("alloc packet failed\n");
		av_bsf_free(&aacbsf);
		avcodec_free_context(&ocodec_ctx);
		avcodec_free_context(&audio_ctx);
		avio_closep(&ctx->pb);
		avio_closep(&fileCtx->pb);
		avformat_free_context(ctx);
		avformat_free_context(fileCtx);
		return;
	}

	AVFrame* output_frame = nullptr;  // 从队列中获取的帧
	int64_t video_pts = 0;            // 视频帧的PTS（显示时间戳）
	int64_t audio_pts = 0;            // 音频帧的PTS
	int64_t last_pts = 0;             // 上一个数据包的PTS（未使用）
	int is_audio = 0;                 // 标志：当前帧是音频还是视频
	// 注意：reorder_buffer 定义了但未使用，可能是遗留代码
	std::set<QueueItem> reorder_buffer;

	// ========== 第十二步：主循环 - 编码和输出 ==========
	for (;;) {
		// 从队列中获取一帧（视频或音频）
		{
			std::unique_lock<std::mutex> lock(mu);
			// 等待队列中有数据（如果队列为空，线程会阻塞在这里）
			cond.wait(lock, []() { return output_queue.size() > 0; });

			// 获取队列头部的帧
			output_frame = output_queue.front().frame;
			is_audio = output_queue.front().is_audio;  // 0=视频，1=音频

			// 计算PTS（Presentation Time Stamp，显示时间戳）
			if (!is_audio) {
				// 视频PTS：帧数 / 帧率，然后转换为流的时间基单位
				// oframes 是全局变量，表示已输出的视频帧数
				video_pts = (oframes * 1.0 / framerate) / (av_q2d(stream->time_base));
				++oframes;  // 视频帧计数加1
			}
			else {
				// 音频PTS：样本数 / 采样率，然后转换为流的时间基单位
				// aframes 是全局变量，表示已输出的音频帧数
				// 1024 是AAC编码器每帧的样本数（固定值）
				audio_pts = (aframes * 1024.0 / samplerate) / (av_q2d(audio->time_base));
				++aframes;  // 音频帧计数加1
			}

			// 从队列中移除已处理的帧
			output_queue.pop_front();
		}

		// ========== 处理视频帧 ==========
		if (!is_audio) {
			// 设置视频帧的图片类型和PTS
			output_frame->pict_type = AV_PICTURE_TYPE_NONE;  // 让编码器自动决定帧类型（I/P/B）
			output_frame->pts = video_pts;

			// 将视频帧发送给编码器
			ret = avcodec_send_frame(ocodec_ctx, output_frame);
			if (ret) {
				printf("encode send video frame %d err %d\n", oframes, ret);
				av_frame_free(&output_frame);
				continue;  // 编码失败，跳过当前帧
			}

			// 释放帧（编码器已经复制了数据）
			av_frame_free(&output_frame);

			// 循环接收编码后的数据包
			// 一个帧可能产生多个数据包（如B帧需要延迟输出）
			while (true) {
				ret = avcodec_receive_packet(ocodec_ctx, packet);
				if (ret) {
					// ret != 0 表示没有更多数据包可接收（需要发送新的帧）
					break;
				}

				// 设置数据包的时间戳和持续时间
				packet->pts = packet->dts = video_pts;  // PTS和DTS相同（简化处理）
				packet->duration = (double)(1.0 / framerate) / av_q2d(stream->time_base);  // 每帧持续时间
				packet->stream_index = 0;  // 视频流索引为0
				last_pts = packet->pts;

				// 写入本地文件（克隆数据包，因为原始数据包会被修改）
				av_interleaved_write_frame(fileCtx, av_packet_clone(packet));

				// 写入RTMP流（推流）
				ret = av_interleaved_write_frame(ctx, packet);
				if (ret < 0) {
					printf("write video packet failed %d\n", ret);
				}

				// 每600帧（约10秒）打印一次进度
				if (oframes % 600 == 0) {
					printf("sent %ld video frames\n", oframes);
				}

				// 释放数据包的引用（不释放packet结构体本身）
				av_packet_unref(packet);
			}
		}
		// ========== 处理音频帧 ==========
		else {
			// 设置音频帧的PTS
			output_frame->pts = audio_pts;

			// 将音频帧发送给编码器
			ret = avcodec_send_frame(audio_ctx, output_frame);
			if (ret) {
				printf("encode send audio frame %ld err %d\n", aframes, ret);
				av_frame_free(&output_frame);
				continue;  // 编码失败，跳过当前帧
			}

			// 释放帧（编码器已经复制了数据）
			av_frame_free(&output_frame);

			// 循环接收编码后的音频数据包
			while (true) {
				ret = avcodec_receive_packet(audio_ctx, packet);
				if (ret) {
					// ret != 0 表示没有更多数据包可接收
					break;
				}

				// ========== 使用BSF转换AAC格式 ==========
				// 将ADTS格式的AAC数据包发送给BSF
				ret = av_bsf_send_packet(aacbsf, packet);
				if (ret) {
					// BSF可能暂时无法处理（缓冲区满），稍后再试
					break;
				}

				// 循环从BSF接收转换后的数据包（ASC格式）
				while (true) {
					ret = av_bsf_receive_packet(aacbsf, packet);
					if (ret) {
						// ret != 0 表示没有更多转换后的数据包
						break;
					}

					// 设置数据包的时间戳和持续时间
					packet->pts = packet->dts = audio_pts;  // PTS和DTS相同
					last_pts = packet->pts;
					// AAC每帧1024个样本，持续时间为 1024/采样率 秒
					packet->duration = (double)(1024.0 / samplerate) / av_q2d(audio->time_base);
					packet->stream_index = 1;  // 音频流索引为1

					// 写入本地文件（克隆数据包）
					av_interleaved_write_frame(fileCtx, av_packet_clone(packet));

					// 写入RTMP流（推流）
					ret = av_interleaved_write_frame(ctx, packet);
					if (ret < 0) {
						printf("write audio packet failed %d\n", ret);
					}
				}

				// 释放数据包的引用
				av_packet_unref(packet);
			}
		}
		// 循环继续，处理下一帧
	}

	// 注意：正常情况下不会执行到这里（无限循环）
	// 如果需要清理资源，应该在这里添加清理代码
}


/**
  * ./live rtmp://example.com/live/output contest "设备测试" "选手1" "选手2"
"选手3" "选手4"
  * 1: 输出流地址
  * 2： 输入流前缀
  * 3: 文本1
  * 4: 文本2
  * 5: 文本3
  * 6: 文本4
  * rtmp://liteavapp.qcloud.com/live/liteavdemoplayerstreamid
  */
int main(int argc, char* argv[]) {
	// 硬编码参数，不再使用命令行参数
	(void)argc;  // 未使用的参数
	(void)argv;  // 未使用的参数

	outputURL = "rtmp://171.17.171.17/live/livestream";  // 输出流地址
	inputPrefix = "contest";  // 输入流前缀
	// texts数组已有默认值，如需修改可在此处更改
	// texts[0] = "设备测试";
	// texts[1] = "参赛选手 1";
	// texts[2] = "参赛选手 2";
	// texts[3] = "参赛选手 3";
	// texts[4] = "参赛选手 4";


	av_log_set_level(AV_LOG_INFO);
	avdevice_register_all(); // 注册所有可用的输入/输出设备
	avformat_network_init(); // 初始化网络库



	blank_screen_generator(); // 生成一个占位画面帧（null_frame），当输入流不可用或出错时作为默认画面使用

	for (int i = 0; i < SESSIONS; ++i) {
		scaler(i); // 缩放滤镜
	}

	compositor(); // 组合滤镜

	std::vector<std::thread> input_threads;
	for (int i = 0; i < SESSIONS; ++i) {
		std::string url = "rtmp://liteavapp.qcloud.com/live/liteavdemoplayerstreamid";
		input_threads.emplace_back(input_stream_handler, url, i);
	}

	// 音频输入线程：从RTMP流读取音频并放入队列
	// 如果不需要音频功能，可以注释掉以下两行
	// std::thread audio(audio_input_handler, "rtmp://localhost/live/audio");

	std::thread output(output_thread);
	std::thread output_io(output_io_thread);
	for (auto&& thread : input_threads) {
		thread.join();
	}

	output.join();
	output_io.join();
	// audio.join();  // 如果注释掉音频线程，也要注释掉这行
	return 0;
}
