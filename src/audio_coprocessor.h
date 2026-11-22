using namespace std;

#include "mos6502/mos6502.h"
#include "SDL_inc.h"

#define ACP_RESET 0
#define ACP_NMI 1
#define ACP_RATE 6

#define AUDIO_RAM_SIZE 4096

typedef struct ACPState {
	uint8_t ram[AUDIO_RAM_SIZE];
	// Whether each byte of it has been written (uninitialized_memory.h).
	bool ram_initialized[AUDIO_RAM_SIZE];
	mos6502 *cpu;
    int16_t irqCounter;
    uint8_t irqRate;
	bool running;
    bool resetting;
    uint8_t dacReg;
    uint16_t clksPerHostSample;
    uint64_t cycles_per_sample;
	uint32_t samples_per_frame;
	uint8_t clkMult;
	SDL_AudioFormat format;
	uint16_t last_irq_cycles;
	uint64_t cycle_counter;
	SDL_AudioDeviceID device;
	int volume;
	bool isMuted;
	bool isEmulationPaused;
} ACPState;

class StateFile;

class AudioCoprocessor {
private:
	//emulated registers/memory
	ACPState state;
	void capture_snapshot();
public:
	static ACPState* singleton_acp_state;
	AudioCoprocessor();
	void StartAudio();
	void ram_write(uint16_t address, uint8_t value);
	uint8_t ram_read(uint16_t address);
	// Whether the CPU reading address is a read of audio RAM nothing has written, to report
	// (UninitializedMemory::IsFirstRead).
	bool is_first_read(uint16_t address);
	// Power-on: no byte of audio RAM counts as written.
	void forget_writes();
	void register_write(uint16_t address, uint8_t value);
	void dump_ram(const char* filename);
	uint16_t get_irq_cycle_count();
	static void fill_audio(void *udata, uint8_t *stream, int len);
	// Saves or loads its RAM, CPU and timing (save_state.h), with the audio thread, which runs
	// it, held off meanwhile. Not the host's audio settings (volume, mute, the device).
	void SerializeState(StateFile& file);
};
