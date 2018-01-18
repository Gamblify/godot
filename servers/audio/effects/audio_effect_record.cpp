/*************************************************************************/
/*  audio_effect_record.cpp                                              */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2018 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2018 Godot Engine contributors (cf. AUTHORS.md)    */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

#include "audio_effect_record.h"
#include "servers/audio_server.h"

void AudioEffectRecordInstance::process(const AudioFrame *p_src_frames, AudioFrame *p_dst_frames, int p_frame_count) {

	//bool is_recording = base->should_record;

	if (!is_recording) {
		return;
	}

	//Add incoming audio frames to the IO ring buffer
	const AudioFrame *src = p_src_frames;
	AudioFrame *rb_buf = ring_buffer.ptrw();
	for (int i = 0; i < p_frame_count; i++) {
		rb_buf[ring_buffer_pos & ring_buffer_mask] = src[i];
		ring_buffer_pos++;
	}
}

bool AudioEffectRecordInstance::process_silence() {
	return true;
}

void AudioEffectRecordInstance::_io_thread_process() {

	//Reset recorder status
	thread_active = true;
	ring_buffer_pos = 0;
	ring_buffer_read_pos = 0;

	//We start a new recording
	_init_recording();
	is_recording = true;

	while (is_recording) {
		//Check: The current recording has been requested to stop
		if (is_recording && !base->should_record) {
			is_recording = false;
		}

		//Case: Frames are remaining in the buffer
		if (ring_buffer_read_pos < ring_buffer_pos) {
			//Read from the buffer into file
			_io_store_buffer();
			//Update the header
			_create_wav_header(ring_buffer_pos); //The ring_buffer_pos will be consistent with the amount of frames written
		}
		//Case: The buffer is empty
		else if (is_recording) {
			//Wait to avoid too much busy-wait
			OS::get_singleton()->delay_usec(500);
		}
	}

	thread_active = false;
}

void AudioEffectRecordInstance::_create_wav_header(int p_frame_count) {
	float sample_rate = AudioServer::get_singleton()->get_mix_rate();
	int sub_chunk_2_size = p_frame_count * 8; //Subchunk2Size = NumSamples * NumChannels * BitsPerSample/8 (This is the size of the data chunk)

	String file_path = base->save_path + save_path_appendage;
	Error err;
	FileAccess *file = FileAccess::open(file_path, FileAccess::READ_WRITE, &err);
	//We override the skipped 44 byte in the beginning of the file with the header
	file->store_string("RIFF"); //ChunkID
	file->store_32(sub_chunk_2_size + 36); //ChunkSize = 36 + SubChunk2Size (size of entire file minus the 8 bits for this and previous header)
	file->store_string("WAVE"); //Format
	file->store_string("fmt "); //Subchunk1ID
	file->store_32(16); //Subchunk1Size = 16
	//file->store_16(1); //AudioFormat = 1 (the value for the PCM format)
	file->store_16(3); //AudioFormat = 3 (the value for IEEE float format)
	file->store_16(2); //NumChannels = 2 (for stereo)
	file->store_32(sample_rate); //SampleRate
	file->store_32(sample_rate * 8); //ByteRate = SampleRate * NumChannels * BitsPerSample/8
	file->store_16(8); //BlockAlign = NumChannels * BitsPerSample/8
	file->store_16(32); //BitsPerSample = 32 (Godot audio frames have float values)
	file->store_string("data"); //Subchunk2ID
	file->store_32(sub_chunk_2_size); //Subchunk2Size
	file->close();
}

void AudioEffectRecordInstance::_init_recording() {
	String file_path = base->save_path + save_path_appendage;

	unsigned int counter = 1;
	while (FileAccess::exists(file_path)) {
		save_path_appendage = "_" + String::num(counter) + ".wav";
		file_path = base->save_path + save_path_appendage;
		counter++;
	}

	Error err;
	FileAccess *file = FileAccess::open(file_path, FileAccess::WRITE, &err);
	//We skip 44 byte to make room for the header
	for (int i = 0; i < 11; i++) {
		file->store_32(0); //4 byte (32 bit) * 11 = 44 byte
	}
	file->close();
}

void AudioEffectRecordInstance::_io_store_buffer() {
	int to_read = ring_buffer_pos - ring_buffer_read_pos;

	String file_path = base->save_path + save_path_appendage;
	Error err;
	FileAccess *file;
	if (FileAccess::exists(file_path)) {
		file = FileAccess::open(file_path, FileAccess::READ_WRITE, &err);
	} else {
		file = FileAccess::open(file_path, FileAccess::WRITE, &err);
	}
	file->seek_end();

	AudioFrame *rb_buf = ring_buffer.ptrw();

	while (to_read) {
		AudioFrame buffered_frame = rb_buf[ring_buffer_read_pos & ring_buffer_mask];
		file->store_float(buffered_frame.l);
		file->store_float(buffered_frame.r);
		ring_buffer_read_pos++;
		to_read--;
	}
	file->close();
}

void AudioEffectRecordInstance::_thread_callback(void *_instance) {

	AudioEffectRecordInstance *aeri = reinterpret_cast<AudioEffectRecordInstance *>(_instance);

	aeri->_io_thread_process();
}

void AudioEffectRecordInstance::init() {
	io_thread = Thread::create(_thread_callback, this);
}

Ref<AudioEffectInstance> AudioEffectRecord::instance() {
	Ref<AudioEffectRecordInstance> ins;
	ins.instance();
	ins->base = Ref<AudioEffectRecord>(this);
	ins->is_recording = false;
	ins->save_path_appendage = ".wav";

	//Re-using the buffer size calculations from audio_effect_delay.cpp
	float ring_buffer_max_size = IO_BUFFER_SIZE_MS;
	ring_buffer_max_size /= 1000.0; //convert to seconds
	ring_buffer_max_size *= AudioServer::get_singleton()->get_mix_rate();

	int ringbuff_size = ring_buffer_max_size;

	int bits = 0;

	while (ringbuff_size > 0) {
		bits++;
		ringbuff_size /= 2;
	}

	ringbuff_size = 1 << bits;
	ins->ring_buffer_mask = ringbuff_size - 1;
	ins->ring_buffer_pos = 0;

	ins->ring_buffer.resize(ringbuff_size);

	ins->ring_buffer_read_pos = 0;

	ensure_thread_stopped();
	current_instance = ins;
	if (should_record) {
		ins->init();
	}

	return ins;
}

void AudioEffectRecord::ensure_thread_stopped() {
	should_record = false;
	if (current_instance != 0 && current_instance->thread_active) {
		Thread::wait_to_finish(current_instance->io_thread);
	}
}

void AudioEffectRecord::set_save_path(String p_path) {
	save_path = p_path;
	if (save_path.right(p_path.length() - 4) == ".wav") {
		save_path = p_path.left(p_path.length() - 4);
	}
}

String AudioEffectRecord::get_save_path() const {
	return save_path + ".wav";
}

void AudioEffectRecord::set_should_record(bool p_record) {
	if (p_record) {
		ensure_thread_stopped();
		current_instance->init();
	}

	should_record = p_record;
}

bool AudioEffectRecord::get_should_record() const {
	return should_record;
}

void AudioEffectRecord::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_save_path", "path"), &AudioEffectRecord::set_save_path);
	ClassDB::bind_method(D_METHOD("get_save_path"), &AudioEffectRecord::get_save_path);
	ClassDB::bind_method(D_METHOD("set_should_record", "record"), &AudioEffectRecord::set_should_record);
	ClassDB::bind_method(D_METHOD("get_should_record"), &AudioEffectRecord::get_should_record);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "save_path", PROPERTY_HINT_FILE, "*.wav"), "set_save_path", "get_save_path");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "should_record"), "set_should_record", "get_should_record");
}

AudioEffectRecord::AudioEffectRecord() {
	save_path = "";
}
