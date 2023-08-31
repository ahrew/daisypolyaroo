#include "daisysp.h"
#include "daisy_seed.h"
#include <algorithm>

using namespace daisysp;
using namespace daisy;
using namespace daisy::seed;

class Voice
{
  public:
    Voice() {}
    ~Voice() {}
    void Init(float samplerate)
    {
        active_ = false;
        osc_.Init(samplerate);
        osc_.SetAmp(0.75f);
        osc_.SetWaveform(Oscillator::WAVE_POLYBLEP_SQUARE);
        // default envelope settings
        env_.Init(samplerate);
        env_.SetSustainLevel(0.5f); 
        env_.SetTime(ADSR_SEG_ATTACK, 1.01f);
        env_.SetTime(ADSR_SEG_DECAY, 0.005f);
        env_.SetTime(ADSR_SEG_RELEASE, 0.2f);
    }

    float Process()
    {
        if(active_)
        {           
            float sig, amp;
            amp = env_.Process(env_gate_);
            if(!env_.IsRunning())
                active_ = false;
            sig = osc_.Process();
            return sig * (velocity_ / 127.f) * amp;
        }
        return 0.f;
    }

    void OnNoteOn(float note, float velocity, float a, float d, float s, float r)
    {

        atk_      = a;
        dcy_      = d;
        sus_      = s;
        rel_      = r;
        env_.SetSustainLevel(sus_);
        env_.SetTime(ADSR_SEG_ATTACK, atk_);
        env_.SetTime(ADSR_SEG_DECAY, dcy_);
        env_.SetTime(ADSR_SEG_RELEASE, rel_);        
        note_     = note;
        velocity_ = velocity;
        osc_.SetFreq(mtof(note_));
        active_   = true;
        env_gate_ = true;
    }

    void OnNoteOff() { env_gate_ = false; }

    inline bool  IsActive() const { return active_; }
    inline float GetNote() const { return note_; }

  private:
    Oscillator osc_;
    Adsr       env_;
    float      note_, velocity_;
    bool       active_;
    bool       env_gate_;
    float atk_, dcy_, sus_, rel_;
};

template <size_t max_voices>
class VoiceManager
{
  public:
    VoiceManager() {}
    ~VoiceManager() {}

    void Init(float samplerate)
    {
        for(size_t i = 0; i < max_voices; i++)
        {
            voices[i].Init(samplerate);
        }
    }

    float Process()
    {
        float sum;
        sum = 0.f;
        for(size_t i = 0; i < max_voices; i++)
        {
            sum += voices[i].Process();
        }
        return sum;
    }

    void OnNoteOn(float notenumber, float velocity, float a, float d, float s, float r)
    {
        Voice *v = FindFreeVoice();
        if(v == NULL)
            return;
        v->OnNoteOn(notenumber, velocity, a, d, s, r);
    }

    void OnNoteOff(float notenumber, float velocity)
    {
        for(size_t i = 0; i < max_voices; i++)
        {
            Voice *v = &voices[i];
            if(v->IsActive() && v->GetNote() == notenumber)
            {
                v->OnNoteOff();
            }
        }
    }

    void FreeAllVoices()
    {
        for(size_t i = 0; i < max_voices; i++)
        {
            voices[i].OnNoteOff();
        }
    }

  private:
    Voice  voices[max_voices];
    Voice *FindFreeVoice()
    {
        Voice *v = NULL;
        for(size_t i = 0; i < max_voices; i++)
        {
            if(!voices[i].IsActive())
            {
                v = &voices[i];
                break;
            }
        }
        return v;
    }
};

static DaisySeed hw;
static ReverbSc verb;
static Svf      filt;
MidiUartHandler midi;
static VoiceManager<8> voice_handler;

enum AdcChannel {
   atkKnob = 0, // voice envelope attack
   dcyKnob,     // voice envelope decay
   susKnob,     // voice envelope sustain
   relKnob,     // voice envelope release
   NUM_ADC_CHANNELS
};

float atkValue, dcyValue, susValue, relValue; 


static void AudioCallback(const float * const*in, float **out, unsigned int size)
{
    // Assign Output Buffers
    float *out_left = out[0];
    float *out_right = out[1];
    // float dry = 0.0f, send = 0.0f, wetl = 0.0f, wetr = 0.0f; // Effects Vars
    for(size_t sample = 0; sample < size; sample++)
    {
        // filt.Process(voice_handler.Process());
        // // get dry sample from the state of the voices
        // dry  = filt.Low() * 0.5f; 
        // // run an attenuated dry signal through the reverb
        // send = dry * 0.45f;
        // verb.Process(send, send, &wetl, &wetr);
        // // sum the dry oscillator and processed reverb signal
        // out_left[sample]  = dry + wetl;
        // out_right[sample] = dry + wetr;

        // debug simple output
        out_left[sample]  = voice_handler.Process();
        out_right[sample] = voice_handler.Process();
    }
}

// Typical Switch case for Message Type.
void HandleMidiMessage(MidiEvent m, float a, float d, float s, float r)
{
    switch(m.type)
    {
        case NoteOn:
        {
            NoteOnEvent p = m.AsNoteOn();
            // Note Off can come in as Note On w/ 0 Velocity
            if(p.velocity == 0.f)
            {
                voice_handler.OnNoteOff(p.note, p.velocity);
            }
            else
            {
                voice_handler.OnNoteOn(p.note, p.velocity, a, d, s, r);
            }
        }
        break;
        case NoteOff:
        {
            NoteOnEvent p = m.AsNoteOn();
            voice_handler.OnNoteOff(p.note, p.velocity);
        }
        break;
        default: break;
    }
}

int main(void)
{
    // initialize seed hardware and daisysp modules
    float sample_rate;
    hw.Configure();
    hw.Init();
    
    AdcChannelConfig my_adc_config[NUM_ADC_CHANNELS];
    my_adc_config[atkKnob].InitSingle(A0);
    my_adc_config[dcyKnob].InitSingle(A1);
    my_adc_config[susKnob].InitSingle(A2);
    my_adc_config[relKnob].InitSingle(A3);
    hw.adc.Init(my_adc_config, NUM_ADC_CHANNELS);
    hw.adc.Start();

    sample_rate = hw.AudioSampleRate();
    MidiUartHandler::Config midi_config;
    midi.Init(midi_config);

    filt.Init(sample_rate);
    filt.SetFreq(6000.f);
    filt.SetRes(0.6f);
    filt.SetDrive(0.8f);

    verb.Init(sample_rate);
    verb.SetFeedback(0.95f);
    verb.SetLpFreq(5000.0f);

    voice_handler.Init(sample_rate);

    // start callback
    hw.StartAudio(AudioCallback);
    midi.StartReceive();

    while(1) 
    {
		atkValue = hw.adc.GetFloat(atkKnob);
        dcyValue = hw.adc.GetFloat(dcyKnob);
        susValue = hw.adc.GetFloat(susKnob);
        relValue = hw.adc.GetFloat(relKnob);
		midi.Listen();
        while(midi.HasEvents())
        {
            HandleMidiMessage(midi.PopEvent(), atkValue, dcyValue, susValue, relValue);
        }
    }
}
