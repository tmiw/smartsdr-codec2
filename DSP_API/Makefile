CFLAGS = -I. -ISmartSDR_Interface -Icodec2/src
OBJS =	SmartSDR_Interface/cmd_basics.o \
	SmartSDR_Interface/cmd_engine.o \
	SmartSDR_Interface/discovery_client.o \
	SmartSDR_Interface/hal_buffer.o \
	SmartSDR_Interface/hal_listener.o \
	SmartSDR_Interface/hal_vita.o \
	SmartSDR_Interface/io_utils.o \
	SmartSDR_Interface/sched_waveform.o \
	SmartSDR_Interface/smartsdr_dsp_api.o \
	SmartSDR_Interface/status_processor.o \
	SmartSDR_Interface/traffic_cop.o \
	SmartSDR_Interface/utils.o \
	SmartSDR_Interface/vita_output.o \
	main.o \
	resampler.o \
	circular_buffer.o \

freedv: $(OBJS) libcodec2.a
	$(CC) -static -o freedv $(OBJS) -Lcodec2/build/src -lcodec2 -lm -pthread
	echo "Done!"

libcodec2.a:
	@mkdir -p codec2/build
	cd codec2/build && \
	cmake -DBUILD_SHARED_LIBS=OFF .. && \
	make

# Other Targets
clean:
	-$(RM) $(EXECUTABLES) $(OBJS) $(C_DEPS) freedv 
	-@echo ' '
