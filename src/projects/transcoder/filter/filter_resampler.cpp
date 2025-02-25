//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================

#include "filter_resampler.h"

#include <base/ovlibrary/ovlibrary.h>

#include "../transcoder_private.h"

FilterResampler::FilterResampler()
{
	_frame = ::av_frame_alloc();

	_outputs = ::avfilter_inout_alloc();
	_inputs = ::avfilter_inout_alloc();

	_input_buffer.SetAlias("Input queue of media resampler filter");
	_input_buffer.SetThreshold(100);

	OV_ASSERT2(_frame != nullptr);
	OV_ASSERT2(_inputs != nullptr);
	OV_ASSERT2(_outputs != nullptr);
}

FilterResampler::~FilterResampler()
{
	Stop();

	OV_SAFE_FUNC(_frame, nullptr, ::av_frame_free, &);

	OV_SAFE_FUNC(_inputs, nullptr, ::avfilter_inout_free, &);
	OV_SAFE_FUNC(_outputs, nullptr, ::avfilter_inout_free, &);

	OV_SAFE_FUNC(_filter_graph, nullptr, ::avfilter_graph_free, &);

	_input_buffer.Clear();
}

bool FilterResampler::Configure(const std::shared_ptr<MediaTrack> &input_track, const std::shared_ptr<MediaTrack> &output_track)
{
	_input_track = input_track;
	_output_track = output_track;

	const AVFilter *abuffersrc = ::avfilter_get_by_name("abuffer");
	const AVFilter *abuffersink = ::avfilter_get_by_name("abuffersink");
	int ret;

	_filter_graph = ::avfilter_graph_alloc();

	if ((_filter_graph == nullptr) || (_inputs == nullptr) || (_outputs == nullptr))
	{
		logte("Could not allocate variables for filter graph: %p, %p, %p", _filter_graph, _inputs, _outputs);
		return false;
	}

	// Limit the number of filter threads to 1. I think 1 thread is usually enough for audio filtering processing.
	_filter_graph->nb_threads = 1;

	AVRational input_timebase = ffmpeg::Conv::TimebaseToAVRational(input_track->GetTimeBase());
	AVRational output_timebase = ffmpeg::Conv::TimebaseToAVRational(output_track->GetTimeBase());

	_scale = ::av_q2d(::av_div_q(input_timebase, output_timebase));

	if (::isnan(_scale))
	{
		logte("Invalid timebase: input: %d/%d, output: %d/%d",
			  input_timebase.num, input_timebase.den,
			  output_timebase.num, output_timebase.den);

		return false;
	}

	// Prepare filters
	//
	// Filter graph:
	//     [abuffer] -> [aresample] -> [asetnsamples] -> [aformat] -> [asettb] -> [abuffersink]

	// Prepare the input parameter
	std::vector<ov::String> src_params = {
		ov::String::FormatString("time_base=%s", input_track->GetTimeBase().GetStringExpr().CStr()),
		ov::String::FormatString("sample_rate=%d", input_track->GetSampleRate()),
		ov::String::FormatString("sample_fmt=%s", input_track->GetSample().GetName()),
		ov::String::FormatString("channel_layout=%s", input_track->GetChannel().GetName())};

	ov::String src_args = ov::String::Join(src_params, ":");

	ret = ::avfilter_graph_create_filter(&_buffersrc_ctx, abuffersrc, "in", src_args, nullptr, _filter_graph);
	if (ret < 0)
	{
		logte("Could not create audio buffer source filter for resampling: %d", ret);
		return false;
	}

	// Prepare output filters
	std::vector<ov::String> filters = {
		ov::String::FormatString("asettb=%s", output_track->GetTimeBase().GetStringExpr().CStr()),
		ov::String::FormatString("aresample=async=1000"),
		ov::String::FormatString("aresample=%d", output_track->GetSampleRate()),
		ov::String::FormatString("aformat=sample_fmts=%s:channel_layouts=%s", output_track->GetSample().GetName(), output_track->GetChannel().GetName()),
		ov::String::FormatString("asetnsamples=n=%d", output_track->GetAudioSamplesPerFrame())};

	ov::String output_filters = ov::String::Join(filters, ",");

	ret = ::avfilter_graph_create_filter(&_buffersink_ctx, abuffersink, "out", nullptr, nullptr, _filter_graph);
	if (ret < 0)
	{
		logte("Could not create audio buffer sink filter for resampling: %d", ret);
		return false;
	}

	_outputs->name = ::av_strdup("in");
	_outputs->filter_ctx = _buffersrc_ctx;
	_outputs->pad_idx = 0;
	_outputs->next = nullptr;

	_inputs->name = ::av_strdup("out");
	_inputs->filter_ctx = _buffersink_ctx;
	_inputs->pad_idx = 0;
	_inputs->next = nullptr;

	if ((_outputs->name == nullptr) || (_inputs->name == nullptr))
	{
		return false;
	}

	ret = ::avfilter_graph_parse_ptr(_filter_graph, output_filters, &_inputs, &_outputs, nullptr);
	if (ret < 0)
	{
		logte("Could not parse filter string for resampling: %d (%s)", ret, output_filters.CStr());
		return false;
	}

	ret = ::avfilter_graph_config(_filter_graph, nullptr);
	if (ret < 0)
	{
		logte("Could not validate filter graph for resampling: %d", ret);
		return false;
	}

	logti("Resampler is enabled for track #%u using parameters. input: %s / outputs: %s", input_track->GetId(), src_args.CStr(), output_filters.CStr());

	return true;
}

bool FilterResampler::Start()
{
	// Generates a thread that reads and encodes frames in the input_buffer queue and places them in the output queue.
	try
	{
		_kill_flag = false;

		_thread_work = std::thread(&FilterResampler::FilterThread, this);
		pthread_setname_np(_thread_work.native_handle(), "Resampler");
	}
	catch (const std::system_error &e)
	{
		_kill_flag = true;

		logte("Failed to start transcode resample filter thread.");

		return false;
	}

	return true;
}

void FilterResampler::Stop()
{
	_kill_flag = true;

	_input_buffer.Stop();

	if (_thread_work.joinable())
	{
		_thread_work.join();
		logtd("resampler filter thread has ended");
	}
}

void FilterResampler::FilterThread()
{
	logtd("Start resampler filter thread.");

	while (!_kill_flag)
	{
		auto obj = _input_buffer.Dequeue();
		if (obj.has_value() == false)
			continue;

		auto media_frame = std::move(obj.value());

		auto av_frame = ffmpeg::Conv::ToAVFrame(cmn::MediaType::Video, media_frame);
		if (!av_frame)
		{
			logte("Could not allocate the frame data");
			break;
		}

		int ret = ::av_buffersrc_add_frame_flags(_buffersrc_ctx, av_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
		if (ret < 0)
		{
			logte("An error occurred while feeding the audio filtergraph: pts: %lld, linesize: %d, srate: %d, layout: %d, channels: %d, format: %d, rq: %d", _frame->pts, _frame->linesize[0], _frame->sample_rate, _frame->channel_layout, _frame->channels, _frame->format, _input_buffer.Size());
			continue;
		}

		while (true)
		{
			int ret = ::av_buffersink_get_frame(_buffersink_ctx, _frame);

			if (ret == AVERROR(EAGAIN))
			{
				break;
			}
			else if (ret == AVERROR_EOF)
			{
				logte("Error receiving a packet for decoding : AVERROR_EOF");
				break;
			}
			else if (ret < 0)
			{
				logte("Error receiving a packet for decoding : %d", ret);
				break;
			}
			else
			{
				auto output_frame = ffmpeg::Conv::ToMediaFrame(cmn::MediaType::Audio, _frame);
				::av_frame_unref(_frame);
				if (output_frame == nullptr)
				{
					logte("Could not allocate the frame data");
					continue;
				}

				if (_complete_handler)
				{
					_complete_handler(std::move(output_frame));
				}
			}
		}
	}
}

int32_t FilterResampler::SendBuffer(std::shared_ptr<MediaFrame> buffer)
{
	_input_buffer.Enqueue(std::move(buffer));

	return 0;
}
