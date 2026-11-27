/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 1993-2012, The Open Group. All rights reserved.
 *
 * These libraries and programs are free software; you can
 * redistribute them and/or modify them under the terms of the GNU
 * Lesser General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * These libraries and programs are distributed in the hope that
 * they will be useful, but WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with these libraries and programs; if not, write
 * to the Free Software Foundation, Inc., 51 Franklin Street, Fifth
 * Floor, Boston, MA 02110-1301 USA
 */
/************************************<+>*************************************
 ****************************************************************************
 **
 **   File:        Audio.c
 **
 **   Project:     DT 3.0
 **
 **   Description: Controls the Dtstyle Audio / Beep dialog (Classic Tone
 **                and General MIDI Note system bell, live Test button, and
 **                Sound & MIDI Control Panel integration)
 **
 **  (c) Copyright Hewlett-Packard Company, 1990.
 **
 ****************************************************************************
 ************************************<+>*************************************/
/* $XConsortium: Audio.c /main/4 1995/10/30 13:07:24 rswiston $ */

#include <X11/Xlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <Xm/MwmUtil.h>
#include <Xm/XmP.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/RowColumn.h>
#include <Xm/Scale.h>
#include <Xm/VendorSEP.h>
#include <Xm/LabelG.h>

#include <Dt/DialogBox.h>
#include <Dt/Icon.h>
#include <Dt/Message.h>
#include <Dt/HourGlass.h>
#include <Dt/Wsm.h>

#include "Help.h"
#include "Main.h"
#include "SaveRestore.h"
#include "Protocol.h"
#include "Audio.h"

#define VOLUME_MAX          100
#define SCALE_WIDTH         265

typedef struct {
    const char *label;
    int gm_program;
} GmBellVoice;

static const GmBellVoice g_gmVoices[] = {
    { "Tubular Bells",        14 },
    { "Vibraphone",           11 },
    { "Celesta",               8 },
    { "Glockenspiel",          9 },
    { "Marimba",              12 },
    { "Acoustic Grand Piano",  0 },
    { "Kalimba",             108 },
    { "Woodblock",           115 },
};
#define NUM_GM_VOICES ((int)(sizeof(g_gmVoices) / sizeof(g_gmVoices[0])))

typedef struct {
    Widget      pictLabel;
    Widget      testBtn;
    Widget      systemDefault;
    Widget      soundMidiBtn;
    Widget      modeLabGad;
    Widget      modeOption;
    Widget      modePulldown;
    Widget      modeToneBtn;
    Widget      modeMidiBtn;
    Widget      voiceLabGad;
    Widget      voiceOption;
    Widget      voicePulldown;
    Widget      voiceBtns[NUM_GM_VOICES];
    Widget      volLabGad;
    Widget      toneLabGad;
    Widget      durLabGad;
    Widget      volScale;
    Widget      toneScale;
    Widget      durScale;
    Boolean     midiMode;
    int         gmProgram;
    Boolean     origMidiMode;
    int         origGmProgram;
    Boolean     systemDefaultFlag;
    XKeyboardState values;
} Audio;

static Audio audio;
static saveRestore save = {FALSE, 0, {{0}}};

static Widget build_audioDlg( Widget shell ) ;
static void formLayoutCB    ( Widget, XtPointer, XtPointer ) ;
static void _DtmapCB_audioDlg( Widget, XtPointer, XtPointer ) ;
static int getValue         ( Widget w ) ;
static void valueChangedCB  ( Widget, XtPointer, XtPointer ) ;
static void testBeepCB      ( Widget, XtPointer, XtPointer ) ;
static void modeChangedCB   ( Widget, XtPointer, XtPointer ) ;
static void voiceChangedCB  ( Widget, XtPointer, XtPointer ) ;
static void systemDefaultCB ( Widget, XtPointer, XtPointer ) ;
static void openSoundMidiCB ( Widget, XtPointer, XtPointer ) ;
static Boolean findCkSoundBinary( char *out_buf, size_t out_len ) ;
static void ButtonCB        ( Widget, XtPointer, XtPointer ) ;
static void loadBeepConf    ( Boolean *out_midi, int *out_gm, int *out_vol, int *out_pitch, int *out_dur ) ;
static void saveBeepConf    ( Boolean midi_mode, int gm_prog, int vol, int pitch, int dur_ms ) ;
static void ringBeepPreview ( void ) ;
static void syncModeVoiceUI ( void ) ;

static int
voiceIndexForProgram(int gm_prog)
{
    for (int i = 0; i < NUM_GM_VOICES; ++i) {
        if (g_gmVoices[i].gm_program == gm_prog)
            return i;
    }
    return 0;
}

static void
loadBeepConf(Boolean *out_midi, int *out_gm, int *out_vol, int *out_pitch, int *out_dur)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.dt/beep.conf", home);
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *v = eq + 1;
        char *end = v + strlen(v);
        while (end > v && isspace((unsigned char)end[-1])) *--end = '\0';

        if (strcasecmp(k, "mode") == 0) {
            if (out_midi) *out_midi = (strcasecmp(v, "midi") == 0) ? True : False;
        } else if (strcasecmp(k, "gm_program") == 0) {
            int g = atoi(v);
            if (out_gm && g >= 0 && g <= 127) *out_gm = g;
        } else if (strcasecmp(k, "volume") == 0) {
            int vol = atoi(v);
            if (out_vol && vol >= 0 && vol <= 100) *out_vol = vol;
        } else if (strcasecmp(k, "pitch") == 0) {
            int hz = atoi(v);
            if (out_pitch && hz >= 82 && hz <= 9000) *out_pitch = hz;
        } else if (strcasecmp(k, "duration_ms") == 0) {
            int d = atoi(v);
            if (out_dur && d >= 100 && d <= 2500) *out_dur = d;
        }
    }
    fclose(fp);
}

static void
saveBeepConf(Boolean midi_mode, int gm_prog, int vol, int pitch, int dur_ms)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    char dir[PATH_MAX], tmp_path[PATH_MAX], final_path[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.dt", home);
    mkdir(dir, 0755);
    snprintf(tmp_path, sizeof(tmp_path), "%s/.dt/beep.conf.tmp.%d", home, (int)getpid());
    snprintf(final_path, sizeof(final_path), "%s/.dt/beep.conf", home);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) return;
    fprintf(fp, "# CDE System Bell configuration (managed by Style Manager - Beep)\n");
    fprintf(fp, "mode=%s\n", midi_mode ? "midi" : "tone");
    fprintf(fp, "gm_program=%d\n", gm_prog);
    fprintf(fp, "volume=%d\n", vol);
    fprintf(fp, "pitch=%d\n", pitch);
    fprintf(fp, "duration_ms=%d\n", dur_ms);
    if (fclose(fp) == 0) {
        rename(tmp_path, final_path);
    } else {
        unlink(tmp_path);
    }
}

/* Fallback pure-C WAV tone generator piped to paplay / pw-cat when ck-sound is absent */
static void
playFallbackToneChild(int vol_pct, int pitch_hz, int dur_ms)
{
    if (vol_pct <= 0) _exit(0);
    if (vol_pct > 100) vol_pct = 100;
    if (pitch_hz < 80) pitch_hz = 80;
    if (pitch_hz > 9000) pitch_hz = 9000;
    if (dur_ms < 50) dur_ms = 50;
    if (dur_ms > 2000) dur_ms = 2000;

    const uint32_t rate = 44100;
    uint32_t frames = (rate * (uint32_t)dur_ms) / 1000u;
    if (frames == 0) _exit(0);
    uint32_t data_bytes = frames * 2u;
    uint32_t riff_size = 36u + data_bytes;

    FILE *pipe_fp = popen("paplay 2>/dev/null || pw-cat -p - 2>/dev/null || aplay -q 2>/dev/null", "w");
    if (!pipe_fp) _exit(0);

    uint8_t hdr[44];
    memcpy(hdr + 0, "RIFF", 4);
    hdr[4] = riff_size & 0xff; hdr[5] = (riff_size >> 8) & 0xff;
    hdr[6] = (riff_size >> 16) & 0xff; hdr[7] = (riff_size >> 24) & 0xff;
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0; /* PCM */
    hdr[20] = 1;  hdr[21] = 0; /* format=1 */
    hdr[22] = 1;  hdr[23] = 0; /* channels=1 */
    hdr[24] = rate & 0xff; hdr[25] = (rate >> 8) & 0xff;
    hdr[26] = (rate >> 16) & 0xff; hdr[27] = (rate >> 24) & 0xff;
    uint32_t byte_rate = rate * 2u;
    hdr[28] = byte_rate & 0xff; hdr[29] = (byte_rate >> 8) & 0xff;
    hdr[30] = (byte_rate >> 16) & 0xff; hdr[31] = (byte_rate >> 24) & 0xff;
    hdr[32] = 2; hdr[33] = 0;  /* block align */
    hdr[34] = 16; hdr[35] = 0; /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    hdr[40] = data_bytes & 0xff; hdr[41] = (data_bytes >> 8) & 0xff;
    hdr[42] = (data_bytes >> 16) & 0xff; hdr[43] = (data_bytes >> 24) & 0xff;
    fwrite(hdr, 1, sizeof(hdr), pipe_fp);

    uint32_t attack = (rate * 6u) / 1000u;
    uint32_t release = (rate * 14u) / 1000u;
    double w = 6.28318530717958647692 * (double)pitch_hz / (double)rate;
    double peak = 22000.0 * ((double)vol_pct / 100.0);

    int16_t chunk[512];
    uint32_t pos = 0;
    while (pos < frames) {
        uint32_t n = frames - pos;
        if (n > 512) n = 512;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t idx = pos + i;
            double env = 1.0;
            if (attack > 0 && idx < attack)
                env = 0.5 * (1.0 - cos(3.14159265358979323846 * (double)idx / (double)attack));
            else if (release > 0 && idx + release >= frames)
                env = 0.5 * (1.0 - cos(3.14159265358979323846 * (double)(frames - 1u - idx) / (double)release));
            double s = peak * env * sin(w * (double)idx);
            chunk[i] = (int16_t)lrint(s);
        }
        fwrite(chunk, sizeof(int16_t), n, pipe_fp);
        pos += n;
    }
    pclose(pipe_fp);
    _exit(0);
}

static void
ringBeepPreview(void)
{
    int vol = getValue(audio.volScale);
    int pitch = getValue(audio.toneScale);
    int dur_ms = getValue(audio.durScale) * 100;
    if (dur_ms < 50) dur_ms = 50;

    char cksound_bin[1024];
    Boolean have_cksound = findCkSoundBinary(cksound_bin, sizeof(cksound_bin));

    pid_t pid = fork();
    if (pid == 0) {
        /* Double-fork so dtstyle never accumulates zombies or blocks UI */
        if (fork() == 0) {
            setsid();
            if (have_cksound) {
                char vol_s[16], pitch_s[16], dur_s[16], gm_s[16];
                snprintf(vol_s, sizeof(vol_s), "%d", vol);
                snprintf(pitch_s, sizeof(pitch_s), "%d", pitch);
                snprintf(dur_s, sizeof(dur_s), "%d", dur_ms);
                snprintf(gm_s, sizeof(gm_s), "%d", audio.gmProgram);
                const char *mode_s = audio.midiMode ? "midi" : "tone";
                char *argv[] = {
                    cksound_bin, "--bell", vol_s, pitch_s, dur_s, (char *)mode_s, gm_s, nullptr
                };
                execv(cksound_bin, argv);
            }
            playFallbackToneChild(vol, pitch, dur_ms);
            _exit(0);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}

static void
syncModeVoiceUI(void)
{
    Boolean have_cksound = hasCkSoundInstalled();
    if (!have_cksound) {
        audio.midiMode = False;
    }
    if (audio.modeMidiBtn) {
        XtSetSensitive(audio.modeMidiBtn, have_cksound);
    }
    if (audio.modeOption) {
        XtVaSetValues(audio.modeOption,
                      XmNmenuHistory, audio.midiMode ? audio.modeMidiBtn : audio.modeToneBtn,
                      NULL);
    }
    int vidx = voiceIndexForProgram(audio.gmProgram);
    if (audio.voiceOption) {
        XtVaSetValues(audio.voiceOption,
                      XmNmenuHistory, audio.voiceBtns[vidx],
                      NULL);
        XtSetSensitive(audio.voiceOption, audio.midiMode ? True : False);
    }
    if (audio.voiceLabGad) {
        XtSetSensitive(audio.voiceLabGad, audio.midiMode ? True : False);
    }
}

void
popup_audioBB(Widget shell)
{
    if (style.audioDialog == nullptr) {
        _DtTurnOnHourGlass(shell);
        build_audioDlg(shell);
        XtManageChild(style.audioDialog);
        _DtTurnOffHourGlass(shell);
    } else {
        XtManageChild(style.audioDialog);
        raiseWindow(XtWindow(XtParent(style.audioDialog)));
    }
    audio.systemDefaultFlag = False;
}

Boolean
hasCkSoundInstalled(void)
{
    char bin[1024];
    return findCkSoundBinary(bin, sizeof(bin));
}

void
popup_soundMidi(void)
{
    openSoundMidiCB(nullptr, nullptr, nullptr);
}

static Widget
build_audioDlg(Widget shell)
{
    int n;
    Arg args[MAX_ARGS];
    XmString button_string[NUM_LABELS];
    XmString string;
    Widget form;
    int count = 0;
    Widget widget_list[20];

    /* Get initial X11 bell values + ~/.dt/beep.conf preferences */
    XGetKeyboardControl(style.display, &audio.values);
    if (audio.values.bell_duration == 0)
        audio.values.bell_duration = 100;

    audio.midiMode = False;
    audio.gmProgram = 14; /* Tubular Bells */
    int conf_vol = (int)audio.values.bell_percent;
    int conf_pitch = (int)audio.values.bell_pitch;
    int conf_dur = (int)audio.values.bell_duration;
    loadBeepConf(&audio.midiMode, &audio.gmProgram, &conf_vol, &conf_pitch, &conf_dur);
    audio.values.bell_percent = conf_vol;
    audio.values.bell_pitch = conf_pitch;
    audio.values.bell_duration = conf_dur;
    audio.origMidiMode = audio.midiMode;
    audio.origGmProgram = audio.gmProgram;

    button_string[0] = CMPSTR((String)_DtOkString);
    button_string[1] = CMPSTR((String)_DtCancelString);
    button_string[2] = CMPSTR((String)_DtHelpString);

    XtSetArg(save.posArgs[save.poscnt], XmNbuttonCount, NUM_LABELS); save.poscnt++;
    XtSetArg(save.posArgs[save.poscnt], XmNbuttonLabelStrings, button_string); save.poscnt++;
    XtSetArg(save.posArgs[save.poscnt], XmNdefaultPosition, False); save.poscnt++;
    style.audioDialog = __DtCreateDialogBoxDialog(shell, "AudioDialog", save.posArgs, save.poscnt);
    XtAddCallback(style.audioDialog, XmNmapCallback, formLayoutCB, nullptr);
    XtAddCallback(style.audioDialog, XmNmapCallback, _DtmapCB_audioDlg, shell);
    XtAddCallback(style.audioDialog, XmNcallback, ButtonCB, nullptr);
    XtAddCallback(style.audioDialog, XmNhelpCallback, HelpRequestCB, (XtPointer)HELP_AUDIO_DIALOG);

    XmStringFree(button_string[0]);
    XmStringFree(button_string[1]);
    XmStringFree(button_string[2]);

    widget_list[0] = _DtDialogBoxGetButton(style.audioDialog, 2);
    n = 0;
    XtSetArg(args[n], XmNautoUnmanage, False); n++;
    XtSetArg(args[n], XmNcancelButton, widget_list[0]); n++;
    XtSetValues(style.audioDialog, args, n);

    n = 0;
    XtSetArg(args[n], XmNtitle, ((char *)GETMESSAGE(6, 7, "Style Manager - Beep"))); n++;
    XtSetArg(args[n], XmNuseAsyncGeometry, True); n++;
    XtSetArg(args[n], XmNmwmFunctions, DIALOG_MWM_FUNC); n++;
    XtSetValues(XtParent(style.audioDialog), args, n);

    n = 0;
    XtSetArg(args[n], XmNhorizontalSpacing, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNverticalSpacing, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNallowOverlap, False); n++;
    XtSetArg(args[n], XmNchildType, XmWORK_AREA); n++;
    form = XmCreateForm(style.audioDialog, "audioForm", args, n);

    n = 0;
    XtSetArg(args[n], XmNfillMode, XmFILL_SELF); n++;
    XtSetArg(args[n], XmNbehavior, XmICON_LABEL); n++;
    XtSetArg(args[n], XmNpixmapForeground, style.secBSCol); n++;
    XtSetArg(args[n], XmNpixmapBackground, style.secTSCol); n++;
    XtSetArg(args[n], XmNstring, NULL); n++;
    XtSetArg(args[n], XmNshadowThickness, 0); n++;
    XtSetArg(args[n], XmNimageName, BEEP_ICON); n++;
    XtSetArg(args[n], XmNtraversalOn, False); n++;
    widget_list[count++] =
    audio.pictLabel = _DtCreateIcon(form, "audiopictLabel", args, n);

    /* Top action buttons: [Sound & MIDI...]  [Test]  [Default] */
    n = 0;
    XtSetArg(args[n], XmNmarginHeight, LB_MARGIN_HEIGHT); n++;
    XtSetArg(args[n], XmNmarginWidth, LB_MARGIN_WIDTH); n++;
    string = CMPSTR(((char *)GETMESSAGE(6, 2, "Default")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] =
    audio.systemDefault = XmCreatePushButtonGadget(form, "systemDefault", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNmarginHeight, LB_MARGIN_HEIGHT); n++;
    XtSetArg(args[n], XmNmarginWidth, LB_MARGIN_WIDTH); n++;
    string = CMPSTR("Test");
    XtSetArg(args[n], XmNlabelString, string); n++;
    widget_list[count++] =
    audio.testBtn = XmCreatePushButtonGadget(form, "testBeepBtn", args, n);
    XmStringFree(string);
    XtAddCallback(audio.testBtn, XmNactivateCallback, testBeepCB, nullptr);

    audio.soundMidiBtn = nullptr;
    {
        char cksound_bin[1024];
        if (findCkSoundBinary(cksound_bin, sizeof(cksound_bin))) {
            n = 0;
            XtSetArg(args[n], XmNmarginHeight, LB_MARGIN_HEIGHT); n++;
            XtSetArg(args[n], XmNmarginWidth, LB_MARGIN_WIDTH); n++;
            string = CMPSTR("Sound & MIDI...");
            XtSetArg(args[n], XmNlabelString, string); n++;
            widget_list[count++] =
            audio.soundMidiBtn = XmCreatePushButtonGadget(form, "soundMidiBtn", args, n);
            XmStringFree(string);
            XtAddCallback(audio.soundMidiBtn, XmNactivateCallback, openSoundMidiCB, nullptr);
        }
    }

    /* Sound Mode selector: Classic Beep (Tone) vs MIDI Note */
    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    string = CMPSTR("Sound");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    widget_list[count++] =
    audio.modeLabGad = XmCreateLabelGadget(form, "modeLabGad", args, n);
    XmStringFree(string);

    audio.modePulldown = XmCreatePulldownMenu(form, "modePulldown", nullptr, 0);
    string = CMPSTR("Classic Beep (Tone)");
    audio.modeToneBtn = XtVaCreateManagedWidget("modeTone", xmPushButtonWidgetClass, audio.modePulldown,
                                                XmNlabelString, string, NULL);
    XmStringFree(string);
    XtAddCallback(audio.modeToneBtn, XmNactivateCallback, modeChangedCB, (XtPointer)0);

    string = CMPSTR("MIDI Note");
    audio.modeMidiBtn = XtVaCreateManagedWidget("modeMidi", xmPushButtonWidgetClass, audio.modePulldown,
                                                XmNlabelString, string, NULL);
    XmStringFree(string);
    XtAddCallback(audio.modeMidiBtn, XmNactivateCallback, modeChangedCB, (XtPointer)1);

    n = 0;
    string = CMPSTR("");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNsubMenuId, audio.modePulldown); n++;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    widget_list[count++] =
    audio.modeOption = XmCreateOptionMenu(form, "modeOption", args, n);
    XmStringFree(string);

    /* MIDI Instrument selector */
    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    string = CMPSTR("Instrument");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    widget_list[count++] =
    audio.voiceLabGad = XmCreateLabelGadget(form, "voiceLabGad", args, n);
    XmStringFree(string);

    audio.voicePulldown = XmCreatePulldownMenu(form, "voicePulldown", nullptr, 0);
    for (int vi = 0; vi < NUM_GM_VOICES; ++vi) {
        char bname[32];
        snprintf(bname, sizeof(bname), "gmVoice%d", vi);
        string = CMPSTR((String)g_gmVoices[vi].label);
        audio.voiceBtns[vi] = XtVaCreateManagedWidget(bname, xmPushButtonWidgetClass, audio.voicePulldown,
                                                      XmNlabelString, string, NULL);
        XmStringFree(string);
        XtAddCallback(audio.voiceBtns[vi], XmNactivateCallback, voiceChangedCB,
                      (XtPointer)(intptr_t)g_gmVoices[vi].gm_program);
    }

    n = 0;
    string = CMPSTR("");
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNsubMenuId, audio.voicePulldown); n++;
    XtSetArg(args[n], XmNmarginWidth, 0); n++;
    widget_list[count++] =
    audio.voiceOption = XmCreateOptionMenu(form, "voiceOption", args, n);
    XmStringFree(string);

    /* Volume, Tone, Duration Sliders */
    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    string = CMPSTR(((char *)GETMESSAGE(6, 3, "Volume")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    widget_list[count++] =
    audio.volLabGad = XmCreateLabelGadget(form, "volLabGad", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNmaximum, VOLUME_MAX); n++;
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNprocessingDirection, XmMAX_ON_RIGHT); n++;
    XtSetArg(args[n], XmNshowValue, True); n++;
    XtSetArg(args[n], XmNvalue, audio.values.bell_percent); n++;
    XtSetArg(args[n], XmNhighlightThickness, SCALE_HIGHLIGHT_THICKNESS); n++;
    XtSetArg(args[n], XmNscaleWidth, SCALE_WIDTH); n++;
    widget_list[count++] =
    audio.volScale = XmCreateScale(form, "volScale", args, n);

    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    string = CMPSTR(((char *)GETMESSAGE(6, 4, "Tone")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    widget_list[count++] =
    audio.toneLabGad = XmCreateLabelGadget(form, "toneLabGad", args, n);
    XmStringFree(string);

    n = 0;
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNprocessingDirection, XmMAX_ON_RIGHT); n++;
    XtSetArg(args[n], XmNshowValue, True); n++;
    XtSetArg(args[n], XmNminimum, 82); n++;
    XtSetArg(args[n], XmNmaximum, 4000); n++;
    XtSetArg(args[n], XmNhighlightThickness, SCALE_HIGHLIGHT_THICKNESS); n++;
    XtSetArg(args[n], XmNscaleWidth, SCALE_WIDTH); n++;
    XtSetArg(args[n], XmNvalue, audio.values.bell_pitch > 4000 ? 4000 :
                               (audio.values.bell_pitch < 82 ? 400 : audio.values.bell_pitch)); n++;
    widget_list[count++] =
    audio.toneScale = XmCreateScale(form, "toneScale", args, n);

    n = 0;
    XtSetArg(args[n], XmNalignment, XmALIGNMENT_END); n++;
    string = CMPSTR(((char *)GETMESSAGE(6, 5, "Duration")));
    XtSetArg(args[n], XmNlabelString, string); n++;
    XtSetArg(args[n], XmNmarginHeight, 0); n++;
    widget_list[count++] =
    audio.durLabGad = XmCreateLabelGadget(form, "durLabGad", args, n);
    XmStringFree(string);

    int init_dur = (int)audio.values.bell_duration / 100;
    if (init_dur < 1) init_dur = 1;
    if (init_dur > 25) init_dur = 25;

    n = 0;
    XtSetArg(args[n], XmNdecimalPoints, (short)1); n++;
    XtSetArg(args[n], XmNorientation, XmHORIZONTAL); n++;
    XtSetArg(args[n], XmNprocessingDirection, XmMAX_ON_RIGHT); n++;
    XtSetArg(args[n], XmNshowValue, True); n++;
    XtSetArg(args[n], XmNminimum, 1); n++;
    XtSetArg(args[n], XmNmaximum, 25); n++;
    XtSetArg(args[n], XmNhighlightThickness, SCALE_HIGHLIGHT_THICKNESS); n++;
    XtSetArg(args[n], XmNvalue, init_dur); n++;
    XtSetArg(args[n], XmNscaleWidth, SCALE_WIDTH); n++;
    widget_list[count++] =
    audio.durScale = XmCreateScale(form, "durationScale", args, n);

    XtAddCallback(audio.systemDefault, XmNactivateCallback, systemDefaultCB, nullptr);
    XtAddCallback(audio.volScale, XmNvalueChangedCallback, valueChangedCB, nullptr);
    XtAddCallback(audio.toneScale, XmNvalueChangedCallback, valueChangedCB, nullptr);
    XtAddCallback(audio.durScale, XmNvalueChangedCallback, valueChangedCB, nullptr);

    syncModeVoiceUI();

    XtManageChild(form);
    XtManageChildren(widget_list, count);

    return style.audioDialog;
}

static void
formLayoutCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;

    int n;
    Arg args[MAX_ARGS];
    Dimension ScaleHeight = XtHeight(audio.volScale);
    Dimension LabelHeight = XtHeight(audio.volLabGad);

    Dimension volLabelWidth = XtWidth(audio.volLabGad);
    Dimension toneLabelWidth = XtWidth(audio.toneLabGad);
    Dimension durLabelWidth = XtWidth(audio.durLabGad);
    Dimension modeLabelWidth = XtWidth(audio.modeLabGad);
    Dimension voiceLabelWidth = XtWidth(audio.voiceLabGad);

    Dimension MaxLabelWidth = volLabelWidth;
    if (toneLabelWidth > MaxLabelWidth) MaxLabelWidth = toneLabelWidth;
    if (durLabelWidth > MaxLabelWidth) MaxLabelWidth = durLabelWidth;
    if (modeLabelWidth > MaxLabelWidth) MaxLabelWidth = modeLabelWidth;
    if (voiceLabelWidth > MaxLabelWidth) MaxLabelWidth = voiceLabelWidth;

    Dimension TopOffset = style.verticalSpacing + ScaleHeight - LabelHeight;
    Dimension LeftOffset;

    /* Picture Label */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNtopOffset, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_NONE); n++;
    XtSetValues(audio.pictLabel, args, n);

    /* Default button (far right) */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNtopOffset, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightOffset, style.horizontalSpacing); n++;
    XtSetValues(audio.systemDefault, args, n);

    /* Test button (left of Default) */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNtopOffset, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNrightWidget, audio.systemDefault); n++;
    XtSetArg(args[n], XmNrightOffset, 8); n++;
    XtSetValues(audio.testBtn, args, n);

    /* Sound & MIDI... button (left of Test) */
    if (audio.soundMidiBtn != nullptr) {
        n = 0;
        XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
        XtSetArg(args[n], XmNtopOffset, style.verticalSpacing); n++;
        XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE); n++;
        XtSetArg(args[n], XmNleftAttachment, XmATTACH_NONE); n++;
        XtSetArg(args[n], XmNrightAttachment, XmATTACH_WIDGET); n++;
        XtSetArg(args[n], XmNrightWidget, audio.testBtn); n++;
        XtSetArg(args[n], XmNrightOffset, 8); n++;
        XtSetValues(audio.soundMidiBtn, args, n);
    }

    /* Sound Mode row */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, audio.pictLabel); n++;
    XtSetArg(args[n], XmNtopOffset, style.verticalSpacing + 4); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, audio.modeLabGad); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    XtSetValues(audio.modeOption, args, n);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, audio.modeOption); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget, audio.modeOption); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    LeftOffset = style.horizontalSpacing + (MaxLabelWidth - modeLabelWidth);
    XtSetArg(args[n], XmNleftOffset, LeftOffset); n++;
    XtSetValues(audio.modeLabGad, args, n);

    /* MIDI Instrument row */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, audio.modeOption); n++;
    XtSetArg(args[n], XmNtopOffset, 4); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, audio.voiceLabGad); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    XtSetValues(audio.voiceOption, args, n);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, audio.voiceOption); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget, audio.voiceOption); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    LeftOffset = style.horizontalSpacing + (MaxLabelWidth - voiceLabelWidth);
    XtSetArg(args[n], XmNleftOffset, LeftOffset); n++;
    XtSetValues(audio.voiceLabGad, args, n);

    /* Volume Label & Scale */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, audio.voiceOption); n++;
    XtSetArg(args[n], XmNtopOffset, TopOffset); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    LeftOffset = style.horizontalSpacing + (MaxLabelWidth - volLabelWidth);
    XtSetArg(args[n], XmNleftOffset, LeftOffset); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_NONE); n++;
    XtSetValues(audio.volLabGad, args, n);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget, audio.volLabGad); n++;
    XtSetArg(args[n], XmNbottomOffset, 0); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, audio.volLabGad); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetValues(audio.volScale, args, n);

    /* Tone Label & Scale */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, audio.volScale); n++;
    XtSetArg(args[n], XmNtopOffset, TopOffset); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    LeftOffset = style.horizontalSpacing + (MaxLabelWidth - toneLabelWidth);
    XtSetArg(args[n], XmNleftOffset, LeftOffset); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_NONE); n++;
    XtSetValues(audio.toneLabGad, args, n);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget, audio.toneLabGad); n++;
    XtSetArg(args[n], XmNbottomOffset, 0); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, audio.toneLabGad); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetValues(audio.toneScale, args, n);

    /* Duration Label & Scale */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNtopWidget, audio.toneScale); n++;
    XtSetArg(args[n], XmNtopOffset, TopOffset); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomOffset, style.verticalSpacing); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    LeftOffset = style.horizontalSpacing + (MaxLabelWidth - durLabelWidth);
    XtSetArg(args[n], XmNleftOffset, LeftOffset); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_NONE); n++;
    XtSetValues(audio.durLabGad, args, n);

    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_NONE); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_OPPOSITE_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget, audio.durLabGad); n++;
    XtSetArg(args[n], XmNbottomOffset, 0); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, audio.durLabGad); n++;
    XtSetArg(args[n], XmNleftOffset, style.horizontalSpacing); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetValues(audio.durScale, args, n);

    XtRemoveCallback(style.audioDialog, XmNmapCallback, formLayoutCB, nullptr);
}

static void
_DtmapCB_audioDlg(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)call_data;
    static int first_time = 1;
    int n;
    Arg args[MAX_ARGS];

    if (first_time) {
        DtWsmRemoveWorkspaceFunctions(style.display, XtWindow(XtParent(w)));
        if (!save.restoreFlag)
            putDialog((Widget)client_data, w);
        first_time = 0;
    } else {
        XGetKeyboardControl(style.display, &audio.values);
        if (audio.values.bell_duration == 0)
            audio.values.bell_duration = 100;

        int conf_vol = (int)audio.values.bell_percent;
        int conf_pitch = (int)audio.values.bell_pitch;
        int conf_dur = (int)audio.values.bell_duration;
        loadBeepConf(&audio.midiMode, &audio.gmProgram, &conf_vol, &conf_pitch, &conf_dur);
        audio.values.bell_percent = conf_vol;
        audio.values.bell_pitch = conf_pitch;
        audio.values.bell_duration = conf_dur;
        audio.origMidiMode = audio.midiMode;
        audio.origGmProgram = audio.gmProgram;

        n = 0;
        XtSetArg(args[n], XmNvalue, audio.values.bell_percent); n++;
        XtSetValues(audio.volScale, args, n);

        int pitch_v = audio.values.bell_pitch;
        if (pitch_v < 82) pitch_v = 400;
        if (pitch_v > 4000) pitch_v = 4000;
        n = 0;
        XtSetArg(args[n], XmNvalue, pitch_v); n++;
        XtSetValues(audio.toneScale, args, n);

        int dur_v = (int)audio.values.bell_duration / 100;
        if (dur_v < 1) dur_v = 1;
        if (dur_v > 25) dur_v = 25;
        n = 0;
        XtSetArg(args[n], XmNvalue, dur_v); n++;
        XtSetValues(audio.durScale, args, n);

        syncModeVoiceUI();
    }
}

static int
getValue(Widget w)
{
    int value = 0;
    Arg args[1];
    XtSetArg(args[0], XmNvalue, &value);
    XtGetValues(w, args, 1);
    return value;
}

static void
valueChangedCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;

    XKeyboardControl kbdControl;
    int kbdControlMask = KBBellPercent | KBBellPitch | KBBellDuration;
    kbdControl.bell_percent = getValue(audio.volScale);
    kbdControl.bell_pitch = getValue(audio.toneScale);
    kbdControl.bell_duration = getValue(audio.durScale) * 100;
    XChangeKeyboardControl(style.display, kbdControlMask, &kbdControl);

    ringBeepPreview();
    audio.systemDefaultFlag = False;
}

static void
testBeepCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    ringBeepPreview();
}

static void
modeChangedCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    intptr_t mode = (intptr_t)client_data;
    audio.midiMode = (mode != 0) ? True : False;
    audio.systemDefaultFlag = False;
    syncModeVoiceUI();
    ringBeepPreview();
}

static void
voiceChangedCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)call_data;
    audio.gmProgram = (int)(intptr_t)client_data;
    audio.systemDefaultFlag = False;
    ringBeepPreview();
}

static Boolean
findCkSoundBinary(char *out_buf, size_t out_len)
{
    const char *path_env = getenv("PATH");
    if (path_env && path_env[0]) {
        const char *p = path_env;
        while (*p) {
            const char *colon = strchr(p, ':');
            size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
            if (dlen > 0 && dlen + 12 < out_len) {
                snprintf(out_buf, out_len, "%.*s/ck-sound", (int)dlen, p);
                if (access(out_buf, X_OK) == 0)
                    return True;
            }
            if (!colon)
                break;
            p = colon + 1;
        }
    }
    static const char *const fallbacks[] = {
        "/usr/local/bin/ck-sound",
        CDE_INSTALLATION_TOP "/bin/ck-sound",
        "/usr/bin/ck-sound",
        nullptr
    };
    for (int i = 0; fallbacks[i] != nullptr; ++i) {
        if (access(fallbacks[i], X_OK) == 0) {
            snprintf(out_buf, out_len, "%s", fallbacks[i]);
            return True;
        }
    }
    return False;
}

static void
openSoundMidiCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;
    char bin[1024];
    if (!findCkSoundBinary(bin, sizeof(bin)))
        return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            char *argv[] = { bin, nullptr };
            execv(bin, argv);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        waitpid(pid, nullptr, 0);
    }
}

static void
systemDefaultCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)w;
    (void)client_data;
    (void)call_data;

    int n;
    Arg args[1];
    XKeyboardControl kbdControl;
    XKeyboardState kbdState;
    int kbdControlMask = KBBellPercent | KBBellPitch | KBBellDuration;

    kbdControl.bell_percent = -1;
    kbdControl.bell_pitch = -1;
    kbdControl.bell_duration = -1;
    XChangeKeyboardControl(style.display, kbdControlMask, &kbdControl);

    XGetKeyboardControl(style.display, &kbdState);
    if (kbdState.bell_duration == 0) kbdState.bell_duration = 100;

    audio.midiMode = False;
    audio.gmProgram = 14;
    syncModeVoiceUI();

    n = 0;
    XtSetArg(args[n], XmNvalue, kbdState.bell_percent); n++;
    XtSetValues(audio.volScale, args, n);

    n = 0;
    XtSetArg(args[n], XmNvalue, kbdState.bell_pitch > 4000 ? 400 :
                               (kbdState.bell_pitch < 82 ? 400 : kbdState.bell_pitch)); n++;
    XtSetValues(audio.toneScale, args, n);

    int def_dur = (int)kbdState.bell_duration / 100;
    if (def_dur < 1) def_dur = 1;
    n = 0;
    XtSetArg(args[n], XmNvalue, def_dur); n++;
    XtSetValues(audio.durScale, args, n);

    ringBeepPreview();
    audio.systemDefaultFlag = True;
}

static void
ButtonCB(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void)client_data;
    int n;
    Arg args[MAX_ARGS];
    XKeyboardControl kbdControl;
    int kbdControlMask;
    DtDialogBoxCallbackStruct *cb = (DtDialogBoxCallbackStruct *)call_data;

    switch (cb->button_position) {
      case OK_BUTTON: {
          int vol = getValue(audio.volScale);
          int pitch = getValue(audio.toneScale);
          int dur_ms = getValue(audio.durScale) * 100;
          XtUnmanageChild(w);

          saveBeepConf(audio.midiMode, audio.gmProgram, vol, pitch, dur_ms);
          audio.origMidiMode = audio.midiMode;
          audio.origGmProgram = audio.gmProgram;
          audio.values.bell_percent = vol;
          audio.values.bell_pitch = pitch;
          audio.values.bell_duration = dur_ms;

          kbdControlMask = KBBellPercent | KBBellPitch | KBBellDuration;
          kbdControl.bell_percent = vol;
          kbdControl.bell_pitch = pitch;
          kbdControl.bell_duration = dur_ms;
          XChangeKeyboardControl(style.display, kbdControlMask, &kbdControl);

          if (style.smState.smSendSettings) {
              if (audio.systemDefaultFlag && !audio.midiMode) {
                  SmDefaultAudioSettings();
              } else {
                  SmNewAudioSettings(vol, pitch, dur_ms);
              }
          }
          break;
      }

      case CANCEL_BUTTON:
          XtUnmanageChild(w);

          audio.midiMode = audio.origMidiMode;
          audio.gmProgram = audio.origGmProgram;
          syncModeVoiceUI();

          n = 0;
          XtSetArg(args[n], XmNvalue, audio.values.bell_percent); n++;
          XtSetValues(audio.volScale, args, n);

          n = 0;
          XtSetArg(args[n], XmNvalue, audio.values.bell_pitch > 4000 ? 4000 :
                                     (audio.values.bell_pitch < 82 ? 400 : audio.values.bell_pitch)); n++;
          XtSetValues(audio.toneScale, args, n);

          n = 0;
          XtSetArg(args[n], XmNvalue, (int)audio.values.bell_duration / 100 < 1 ? 1 :
                                     (int)audio.values.bell_duration / 100); n++;
          XtSetValues(audio.durScale, args, n);

          kbdControlMask = KBBellPercent | KBBellPitch | KBBellDuration;
          kbdControl.bell_percent = audio.values.bell_percent;
          kbdControl.bell_pitch = audio.values.bell_pitch;
          kbdControl.bell_duration = audio.values.bell_duration;
          XChangeKeyboardControl(style.display, kbdControlMask, &kbdControl);
          break;

      case HELP_BUTTON:
          XtCallCallbacks(style.audioDialog, XmNhelpCallback, (XtPointer)nullptr);
          break;

      default:
          break;
    }
}

void
restoreAudio(Widget shell, XrmDatabase db)
{
    XrmName xrm_name[5];
    XrmRepresentation rep_type;
    XrmValue value;

    xrm_name[0] = XrmStringToQuark("audioDlg");
    xrm_name[2] = 0;

    xrm_name[1] = XrmStringToQuark("x");
    if (XrmQGetResource(db, xrm_name, xrm_name, &rep_type, &value)) {
        XtSetArg(save.posArgs[save.poscnt], XmNx, atoi((char *)value.addr)); save.poscnt++;
        save.restoreFlag = True;
    }

    xrm_name[1] = XrmStringToQuark("y");
    if (XrmQGetResource(db, xrm_name, xrm_name, &rep_type, &value)) {
        XtSetArg(save.posArgs[save.poscnt], XmNy, atoi((char *)value.addr)); save.poscnt++;
    }

    xrm_name[1] = XrmStringToQuark("ismapped");
    if (XrmQGetResource(db, xrm_name, xrm_name, &rep_type, &value) &&
        value.addr != nullptr &&
        strcmp(value.addr, "True") == 0)
        popup_audioBB(shell);
}

void
saveAudio(int fd)
{
    Position x, y;
    char bufr[1024];
    XmVendorShellExtObject vendorExt;
    XmWidgetExtData extData;

    if (style.audioDialog != nullptr) {
        if (XtIsManaged(style.audioDialog))
            sprintf(bufr, "*audioDlg.ismapped: True\n");
        else
            sprintf(bufr, "*audioDlg.ismapped: False\n");

        WRITE_STR2FD(fd, bufr);

        x = XtX(XtParent(style.audioDialog));
        y = XtY(XtParent(style.audioDialog));

        extData = _XmGetWidgetExtData(style.shell, XmSHELL_EXTENSION);
        vendorExt = (XmVendorShellExtObject)extData->widget;
        x -= vendorExt->vendor.xOffset;
        y -= vendorExt->vendor.yOffset;

        snprintf(bufr, sizeof(bufr), "*audioDlg.x: %d\n", x);
        WRITE_STR2FD(fd, bufr);

        snprintf(bufr, sizeof(bufr), "*audioDlg.y: %d\n", y);
        WRITE_STR2FD(fd, bufr);
    }
}
