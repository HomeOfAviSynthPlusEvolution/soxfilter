#include <avisynth.h>
#include "include/sox.h"
#include <algorithm>
#include <vector>

char lasterror[512];
static const char* st_getlasterror() {
  return lasterror;
}

static void output_message(unsigned level, const char *filename, const char *fmt, va_list ap) {
  if (level == 1) {
    sprintf_s(lasterror, sizeof(lasterror), fmt, ap);
  }
}

class SoxFilter : public GenericVideoFilter
{
public:
  SoxFilter(PClip _child, const AVSValue args, IScriptEnvironment *_env);
  virtual ~SoxFilter();
  void __stdcall GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env);
  int convertSpaceToNull(char *outStr);
  void flow_filter(int n, IScriptEnvironment *env);

private:
  bool CanRestart(const char *name);
  std::vector<sox_effect_t *> ste; // Effects
  sox_signalinfo_t *si;
  sox_encodinginfo_t *ei;
  bool allMultichannel; // Does all effects run multichannel
  const int ST_BUFSIZ;
  const int buffer_samples;
  int skipToPrev;
  size_t input_samples_used;
  int *read_buffer;
  int **single_ch_read_buffer; // Used to unpack data.
  __int64 next_start;
};

SoxFilter::SoxFilter(PClip _child, const AVSValue args, IScriptEnvironment *env)
    : GenericVideoFilter(_child),
      ST_BUFSIZ(static_cast<int>(vi.BytesFromAudioSamples(vi.audio_samples_per_second))),
      buffer_samples(vi.audio_samples_per_second * vi.AudioChannels()) {
  // Set input/output
  si = new sox_signalinfo_t;
  ei = new sox_encodinginfo_t { SOX_ENCODING_SIGN2, 32, 0.0, sox_option_no, sox_option_no, sox_option_no, sox_false};
  si->rate = vi.audio_samples_per_second;
  si->channels = vi.AudioChannels();
  si->precision = 32;

  read_buffer = (int *)malloc(ST_BUFSIZ);

  allMultichannel = true; // So far all filters support multichannel.
  skipToPrev = 1;

  char err[512]; // Error buffer
  // Create effects
  int curr_eff = 0;
  AVSValue args_c = args[1]; // OMG: warning C4700: local variable 'args_c' used without having been initialized  ;)

  const int num_args = args_c.ArraySize();

  if (!num_args) {
    env->ThrowError("SoxFilter: No effects specified");
  }

  while (curr_eff < num_args) {
    sox_effect_t *se;
    const sox_effect_handler_t *seh;

    char *arg_str = strdup(args_c[curr_eff].AsString());
    size_t all_len = strlen(arg_str);

    int nargs = convertSpaceToNull(arg_str);

    if (nullptr == (seh = sox_find_effect(arg_str))) {
      sprintf(err, "SoxFilter:  Could not find effect:%s", arg_str);
      env->ThrowError(err);
    }
    if (seh->flags & SOX_EFF_CHAN) {
      sprintf(err, "SoxFilter:  (%s) Cannot run filters that change the number of channels.", arg_str);
      env->ThrowError(err);
    }
    if (seh->flags & SOX_EFF_RATE) {
      sprintf(err, "SoxFilter:  (%s) Cannot run filters that changes the samplerate.", arg_str);
      env->ThrowError(err);
    }
    se = sox_create_effect(seh);
    char **arglist = (char **)malloc((nargs) * sizeof(char *));
    int c_arg = 0;
    for (int i = 1; i < all_len - 1; i++) {
      if (0 == arg_str[i])
        arglist[c_arg++] = arg_str + i + 1;
    }

    if (SOX_SUCCESS != (seh->getopts)(se, nargs, arglist)) {
      sprintf(err, "SoxFilter:  (%s) Parameter error: \n\n%s", arg_str, st_getlasterror());
      sox_delete_effect(se);
      env->ThrowError(err);
    }

    free(arg_str);

    if (!(seh->flags & SOX_EFF_MCHAN) && vi.AudioChannels() > 1) { // Create multiple instances
      std::vector<sox_effect_t *> ste_temp(ste);                            // Copy effects
      ste.clear();

      for (auto &&pos : ste_temp)
      {
        for (int i = 0; i < vi.AudioChannels(); i++) {
          sox_effect_t *temp_ste = (sox_effect_t *)malloc(sizeof(sox_effect_t));
          memcpy(temp_ste, pos, sizeof(sox_effect_t));            // Copy current effect
          temp_ste->obuf = (int *)malloc(ST_BUFSIZ / skipToPrev); // Allocate output buffer
          ste.push_back(temp_ste);                                // Push to back.
        }
        free(pos->obuf); // Delete old output buffer
        sox_delete_effect(pos);         // Delete old effect struct.
      }
      
      allMultichannel = false;
      skipToPrev = vi.AudioChannels();
      si->channels = 1;

      single_ch_read_buffer = (int **)malloc(skipToPrev * sizeof(int *));
      for (int i = 0; i < skipToPrev; i++) {
        single_ch_read_buffer[i] = (int *)malloc(ST_BUFSIZ / skipToPrev); // Single ch buffer.
      }
    }

    // st_updateeffect(new_ste, si, si, new_ste->h->flags);

    if (!allMultichannel) {
      for (int i = 0; i < vi.AudioChannels(); i++) { // Create one for each channel.
        sox_effect_t *te = (sox_effect_t *)malloc(sizeof(sox_effect_t));
        memcpy(te, se, sizeof(sox_effect_t));
        te->obuf = (int *)malloc(ST_BUFSIZ / skipToPrev); // Allocate output buffer
        ste.push_back(te);                                // Push to back.
      }
    } else {
      se->obuf = (int *)malloc(ST_BUFSIZ); // Allocate output buffer
      ste.push_back(se);                   // Push to back.
    }

    curr_eff++;
  } // End while

  // Initialize filters
  for (auto &&pos : ste)
  {
    if (SOX_SUCCESS != (pos->handler.start)(pos)) {
      sprintf(err, "SoxFilter:  (%s) Could not start filter: \n\n%s", pos->handler.name, st_getlasterror());
      env->ThrowError(err);
    }
    
  }
  
  input_samples_used = buffer_samples; // Prepare for read on first filter.
  next_start = 0;
}

/*
 * SoxFilter::~SoxFilter
 *
 * Free allocated memory.
 */
SoxFilter::~SoxFilter() {
  for (auto &&pos : ste)
  {
    if (CanRestart(pos->handler.name))
      (pos->handler.stop)(pos); // Crashes compand on exit
    free(pos->obuf);
    sox_delete_effect(pos);
  }
  
  delete si;
  delete ei;
  free(read_buffer);
  if (!allMultichannel) {
    for (int i = 0; i < skipToPrev; i++) {
      free(single_ch_read_buffer[i]);
    }
    free(single_ch_read_buffer);
  }
}

void __stdcall SoxFilter::GetAudio(void *buf, __int64 start, __int64 count, IScriptEnvironment *env) {
  // Get input samples

  // Everything in SOX is single samples, not accounting for channels.
  size_t needing_samples = static_cast<size_t>(count * vi.AudioChannels());
  size_t delivered_samples = 0;
  int *outbuf = (int *)buf;

  // First we check if we should reinitialize filters.
  std::vector<sox_effect_t *>::iterator pos;
  if (start <= 0 && next_start > 0) {
    pos = ste.begin();
    for (; pos < ste.end(); ++pos) {
      if (CanRestart((*pos)->handler.name))
        ((*pos)->handler.stop)(*pos);
      if (SOX_SUCCESS != ((*pos)->handler.start)(*pos)) {
        char err[512]; // Error buffer
        sprintf(err, "SoxFilter:  (%s) Could not restart filter: \n\n%s", (*pos)->handler.name, st_getlasterror());
        env->ThrowError(err);
      }
      (*pos)->obeg = 0;
      (*pos)->oend = 0;
    }
    input_samples_used = buffer_samples; // Prepare for read on first filter.
    next_start = 0;
  }

  while (needing_samples > 0) {
    if (allMultichannel) {
      pos = ste.end(); // We fetch data from last filter
      pos--;
      // do we have enough for one sample?
      if ((*pos)->oend - (*pos)->obeg > vi.AudioChannels()) {
        // Copy what we can/have.
        size_t copy_samples = std::min<size_t>(needing_samples, (*pos)->oend - (*pos)->obeg);

        memcpy(&outbuf[delivered_samples], &(*pos)->obuf[(*pos)->obeg], copy_samples * sizeof(int));

        delivered_samples += copy_samples;
        needing_samples -= copy_samples;
        (*pos)->obeg += copy_samples;

      } else { // Run filter chain to produce samples.

        for (int i = 0; i < ste.size(); i++) {
          flow_filter(i, env);
        }
      }
    } else {           // NOT all multichannel
      pos = ste.end(); // We fetch data from last filter
      pos -= skipToPrev;
      // Do we have a samples do we need more?
      if ((*pos)->oend - (*pos)->obeg > 0) {
        size_t copy_samples = std::min<size_t>(needing_samples / skipToPrev, (*pos)->oend - (*pos)->obeg);
        int dis = 0;
        for (; pos < ste.end(); pos++) {
          sox_effect_t *eff = *pos;
          int off = dis;
          for (int i = 0; i < copy_samples; i++) {
            outbuf[delivered_samples + off] = eff->obuf[eff->obeg + i];
            off += skipToPrev;
          }
          eff->obeg += copy_samples;
          dis++;
        }
        delivered_samples += copy_samples * skipToPrev;
        needing_samples -= copy_samples * skipToPrev;
      } else { // Run filter chain to produce samples.

        for (int i = 0; i < ste.size(); i++) {
          flow_filter(i, env);
        }
      } // End if no samples available
    }   // End if not ALL filters multichannel.
  }     // end while
}

void SoxFilter::flow_filter(int n, IScriptEnvironment *env) {

  sox_effect_t *e = ste[n];

  if (e->oend == e->obeg) { // We have used all samples, we can reset buffers
    e->obeg = 0;
    e->oend = 0;
  }

  int *inbuf;
  size_t samples_available = 0;

  if (0 == n) {                                 // Should we provide more input samples
    if (input_samples_used == buffer_samples) { // Input buffer used up. Read more samples.
      int count = buffer_samples / vi.AudioChannels();
      child->GetAudio(read_buffer, next_start, count, env);
      next_start += count;
      input_samples_used = 0;

      if (!allMultichannel) { // Distribute what we've read
        int nsamples = buffer_samples / skipToPrev;
        for (int j = 0; j < skipToPrev; j++) {
          inbuf = single_ch_read_buffer[j];
          for (int i = 0; i < nsamples; i++) {
            inbuf[i] = read_buffer[i * skipToPrev + j];
          }
        } // end for j
      }   // end if !allmulti
    }     // end if inputsamples == buffer_samples

    if (allMultichannel) { // Distribute what we've read
      inbuf = &read_buffer[input_samples_used];
      samples_available = (buffer_samples - input_samples_used);
    } else {
      inbuf = single_ch_read_buffer[n];
      inbuf = &inbuf[input_samples_used / skipToPrev];
      samples_available = (buffer_samples - input_samples_used) / skipToPrev;
    }

  } else if (n >= skipToPrev) { // if after first filter, fetch from previous filter
    sox_effect_t *prev = ste[n - skipToPrev];
    inbuf = &prev->obuf[prev->obeg];
    samples_available = prev->oend - prev->obeg;
  }

  // ASSERT: All multichannel intances should behave the same!!! (yes - very, very likely)
  if (!allMultichannel && n < skipToPrev) {
    samples_available = (buffer_samples - input_samples_used) / skipToPrev;
    inbuf = single_ch_read_buffer[n];
    inbuf = &inbuf[input_samples_used / skipToPrev];
  }

  size_t out_size = (buffer_samples / skipToPrev) - e->oend;

  (e->handler.flow)(e, inbuf, &e->obuf[e->oend], &samples_available,
               &out_size); // FIXME: Is using local variables reentrant?

  e->oend += out_size;

  if (!allMultichannel) {
    if (skipToPrev == n + 1) { // When we've reached the last of the first filter, update samples used.
      input_samples_used += samples_available * skipToPrev;
    } else if (n >= skipToPrev) { // If we are after first filter, update previous filter.
      sox_effect_t *prev = ste[n - skipToPrev];
      prev->obeg += samples_available;
    }
  } else if (0 == n) { // If first filter, update samples used
    input_samples_used += samples_available;
  } else { // Update prev done buffer
    sox_effect_t *prev = ste[n - skipToPrev];
    prev->obeg += samples_available;
  }
}

int SoxFilter::convertSpaceToNull(char *outStr) {
  size_t len = strlen(outStr);
  int nreps = 0;
  for (int i = 0; i < len; i++) {
    if (32 == outStr[i]) {
      outStr[i] = 0;
      nreps++;
    }
  }
  return nreps;
}

bool SoxFilter::CanRestart(const char *name) {
  if (!strcmpi(name, "compand")) {
    return false;
  } else if (!strcmpi(name, "mcompand")) {
    return false;
  }
  return true;
}
AVSValue __cdecl Create_SoxFilter(AVSValue args, void *user_data, IScriptEnvironment *env) {
  sox_globals.output_message_handler = output_message;

  AVSValue Ia1[1] = {args[0]};
  auto src_clip = env->Invoke("ConvertAudioTo32bit", AVSValue(Ia1, 1));
  PClip clip = new SoxFilter(src_clip.AsClip(), args, env);
  AVSValue Ia2[1] = {clip};
  return env->Invoke("EnsureVBRMp3Sync", AVSValue(Ia2, 1));
}

const AVS_Linkage *AVS_linkage = NULL;

extern "C" __declspec(dllexport) const
    char *__stdcall AvisynthPluginInit3(IScriptEnvironment *env, AVS_Linkage *linkage) {
  AVS_linkage = linkage;
  env->AddFunction("SoxFilter", "cs+", Create_SoxFilter, NULL);
  return "SoxFilter";
}
