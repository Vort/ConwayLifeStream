#include <fstream>
#include <iostream>
#include <string>

#include <csignal>

#include <chrono>
#include <thread>

#include <random>

extern "C" {
// From ffmpeg-4.4.1-full_build-shared.7z
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

#define PREGENERATE true
#define FRAME_WIDTH 1280
#define FRAME_HEIGHT 720
#define FPS 30

#define GENERATION_DURATION (FPS / 2)
#define SNOW_INTERVAL 20

#define CELL_SIZE 8
#define BOARD_WIDTH (FRAME_WIDTH / CELL_SIZE)
#define BOARD_HEIGHT (FRAME_HEIGHT / CELL_SIZE)

class Board
{
public:
	Board(std::mt19937& mt)
		: mt(mt)
	{
		std::uniform_int_distribution<> d1(0, 1);
		for (int y = 0; y < BOARD_HEIGHT; y++)
		{
			for (int x = 0; x < BOARD_WIDTH; x++)
			{
				if (PREGENERATE)
					cells[y][x] = d1(mt);
				else
					cells[y][x] = 0;
			}
		}
	}

	void InvertCell(int x, int y)
	{
		int xw = x;
		int yw = y;
		WrapCoordinates(xw, yw);
		cells[yw][xw] = 1 - cells[yw][xw];
	}

	void AddSnow()
	{
		std::uniform_int_distribution<> dx(0, BOARD_WIDTH - 1);
		std::uniform_int_distribution<> dy(0, BOARD_HEIGHT - 1);
		std::uniform_int_distribution<> d1(0, 1);

		int sx = dx(mt);
		int sy = dy(mt);

		for (int x = sx - 1; x <= sx + 1; x++)
			for (int y = sy - 1; y <= sy + 1; y++)
				if (d1(mt) == 1)
					InvertCell(x, y);
	}

	void NewGeneration(Board* oldBoard)
	{
		for (int y = 0; y < BOARD_HEIGHT; y++)
		{
			for (int x = 0; x < BOARD_WIDTH; x++)
			{
				int centerCell = 0;
				int neighbours = oldBoard->GetNeighbourInfo(x, y, centerCell);

				if (centerCell == 1)
					cells[y][x] = neighbours == 2 || neighbours == 3;
				else
					cells[y][x] = neighbours == 3;
			}
		}
	}

	void Render(AVFrame* frame)
	{
		uint8_t* data = frame->data[0];
		for (int y = 0; y < FRAME_HEIGHT; y++)
		{
			int cy = y / CELL_SIZE;
			for (int cx = 0; cx < BOARD_WIDTH; cx++)
			{
				uint8_t color = cells[cy][cx] ? 255 : 0;
				for (int c = 0; c < CELL_SIZE; c++)
					*data++ = color;
			}
		}
	}

private:
	// x and y should not overflow
	int GetNeighbourInfo(int x, int y, int& centerCell)
	{
		int neighbours = 0;
		int xm1 = x - 1;
		int ym1 = y - 1;
		int xp1 = x + 1;
		int yp1 = y + 1;
		WrapCoordinates(xm1, ym1);
		WrapCoordinates(xp1, yp1);
		neighbours += cells[ym1][xm1];
		neighbours += cells[ym1][x];
		neighbours += cells[ym1][xp1];
		neighbours += cells[y][xm1];
		neighbours += cells[y][xp1];
		neighbours += cells[yp1][xm1];
		neighbours += cells[yp1][x];
		neighbours += cells[yp1][xp1];
		centerCell = cells[y][x];
		return neighbours;
	}

	void WrapCoordinates(int& x, int& y)
	{
		if (x < 0)
			x = BOARD_WIDTH - 1;
		if (x >= BOARD_WIDTH)
			x = 0;
		if (y < 0)
			y = BOARD_HEIGHT - 1;
		if (y >= BOARD_HEIGHT)
			y = 0;
	}

private:
	std::mt19937& mt;
	int cells[BOARD_HEIGHT][BOARD_WIDTH];
};

static bool write_frame(AVFormatContext* fc, AVCodecContext* cc,
	AVStream* stream, AVFrame *frame, AVPacket *pkt)
{
	int ret;

	ret = avcodec_send_frame(cc, frame);
	if (ret < 0)
	{
		std::cout << "Error sending a frame for encoding" << std::endl;
		return false;
	}

	while (ret >= 0)
	{
		ret = avcodec_receive_packet(cc, pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
		{
			return true;
		}
		else if (ret < 0)
		{
			std::cout << "Error during encoding" << std::endl;
			return false;
		}

		av_packet_rescale_ts(pkt, cc->time_base, stream->time_base);

		int wfret = av_interleaved_write_frame(fc, pkt);
		if (wfret < 0)
		{
			std::cout << "Error while writing frame: " << wfret << std::endl;
			return false;
		}
		av_packet_unref(pkt);
	}
	return true;
}

static void UpdateBoard(Board*& board, Board*& boardNew, int iteration)
{
	boardNew->NewGeneration(board);

	Board* temp = board;
	board = boardNew;
	boardNew = temp;

	if (iteration / GENERATION_DURATION % SNOW_INTERVAL == 0)
		board->AddSnow();
}

void main()
{
	std::string streamUrl;
	std::ifstream fs("stream_url.txt");

	if (fs.fail())
	{
		std::cout << "You need to put ingest endpoint into 'stream_url.txt' file." << std::endl;
		std::cout << "File should contain line like this one:" << std::endl;
		std::cout << "rtmp://waw.contribute.live-video.net/app/live_851531407_98eVk23PZTlZYEGkJvUWcWNmQaLvnp" << std::endl;
		return;
	}
	std::getline(fs, streamUrl);

	AVFormatContext* fc = nullptr;
	int ret = avformat_alloc_output_context2(&fc, nullptr, "flv", streamUrl.c_str());
	if (ret < 0)
	{
		std::cout << "Could not allocate output format context" << std::endl;
		return;
	}

	if (!(fc->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open2(&fc->pb, streamUrl.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
		if (ret < 0)
		{
			std::cout << "Could not open output IO context" << std::endl;
			return;
		}
	}

	const AVCodec* codec = avcodec_find_encoder_by_name("h264_qsv");
	if (!codec)
	{
		std::cout << "Encoder not found" << std::endl;
		return;
	}

	AVStream* stream = avformat_new_stream(fc, codec);

	AVCodecContext* cc = avcodec_alloc_context3(codec);
	if (!cc)
	{
		std::cout << "Could not allocate video codec context" << std::endl;
		return;
	}

	cc->bit_rate = 3000000;
	cc->width = FRAME_WIDTH;
	cc->height = FRAME_HEIGHT;

	const AVRational fpsr = { FPS, 1 };
	cc->framerate = fpsr;
	cc->time_base = av_inv_q(fpsr);

	cc->gop_size = FPS * 2;
	cc->pix_fmt = AV_PIX_FMT_NV12;

	ret = avcodec_parameters_from_context(stream->codecpar, cc);
	if (ret < 0)
	{
		std::cout << "Could not initialize stream codec parameters" << std::endl;
		return;
	}

	AVDictionary* codecOptions = nullptr;
	av_dict_set(&codecOptions, "preset", "medium", 0);

	ret = avcodec_open2(cc, codec, &codecOptions);
	if (ret < 0)
	{
		std::cout << "Could not open video encoder: " << ret << std::endl;
		return;
	}

	stream->codecpar->extradata = cc->extradata;
	stream->codecpar->extradata_size = cc->extradata_size;

	AVFrame* frame = av_frame_alloc();
	if (!frame)
	{
		std::cout << "Could not allocate video frame" << std::endl;
		return;
	}
	frame->format = cc->pix_fmt;
	frame->width = cc->width;
	frame->height = cc->height;

	ret = avformat_write_header(fc, nullptr);
	if (ret < 0)
	{
		std::cout << "Could not write header" << std::endl;
		return;
	}

	ret = av_frame_get_buffer(frame, 0);
	if (ret < 0)
	{
		std::cout << "Could not allocate the video frame data" << std::endl;
		return;
	}

	for (int y = 0; y < cc->height / 2; y++)
		for (int x = 0; x < cc->width; x++)
			frame->data[1][y * frame->linesize[1] + x] = 128;

	AVPacket* pkt = av_packet_alloc();
	if (!pkt)
		return;

	std::random_device rd;
	std::mt19937 mt(rd());
	Board* board = new Board(mt);
	Board* boardNew = new Board(mt);

	static bool shouldExit = false;
	auto l = [](int) { shouldExit = true; };
	std::signal(SIGINT, l);
	std::signal(SIGBREAK, l);

	std::chrono::duration<double, std::milli> frameDuration(1000.0 / FPS);

	std::chrono::high_resolution_clock::time_point t1 =
		std::chrono::high_resolution_clock::now();

	if (PREGENERATE)
		for (int i = 0; i < 1200; i++)
			UpdateBoard(board, boardNew, i);

	for (int i = 0; !shouldExit; i++)
	{
		ret = av_frame_make_writable(frame);
		if (ret < 0)
			return;

		if (i % GENERATION_DURATION == 0)
		{
			if (i != 0)
				UpdateBoard(board, boardNew, i);
			board->Render(frame);
		}

		frame->pts = i;
		if (!write_frame(fc, cc, stream, frame, pkt))
			return;

		std::chrono::high_resolution_clock::time_point t2 =
			std::chrono::high_resolution_clock::now();
		std::chrono::duration<double, std::milli> time_span = t2 - t1;
		if (time_span < frameDuration)
			std::this_thread::sleep_for(frameDuration - time_span);
		t1 += std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(frameDuration);
		if (i != 0 && i % FPS == 0)
			std::cout << ".";
	}

	std::cout << std::endl << "Exiting..." << std::endl;

	av_write_trailer(fc);

	avcodec_free_context(&cc);
	av_frame_free(&frame);
	av_packet_free(&pkt);

	if (!(fc->oformat->flags & AVFMT_NOFILE))
		avio_closep(&fc->pb);

	stream->codecpar->extradata = nullptr;
	stream->codecpar->extradata_size = 0;

	avformat_free_context(fc);

	delete board;
	delete boardNew;
}