/* ============================================================================
 * XOS 音频子系统核心头文件（第 16 册 · 设备驱动 · 音频）V2
 * 完全自研：设备描述符模型 + PC 扬声器（PIT 通道 2 + 端口 0x61）真实发声 +
 * PCI 配置空间扫描（AC97/HDA 虚拟声卡探测）+ PCM 环形缓冲 + 采样率/声道
 * 转换 + 音量/静音 + PIT 音频时钟 + 省电/错误恢复/路由/权限/统计/自检。
 * V2 新增：PCM 录制路径、声道映射与 pan、多流混音、3 段均衡与混响、
 * MIDI 消息队列与合成器（12-TET 音符频率）、USB 音频端点模型、
 * 蓝牙 A2DP/SBC 概要、低延迟水位监控、欠载过载错误恢复、
 * 正弦波生成与校准、24/32 位深扩展。
 * 不依赖任何外部音频核心；全部为自研实现。
 * ========================================================================== */
#ifndef XOS_SOUND_H
#define XOS_SOUND_H

#include "types.h"

#define SND_MAGIC       0x58534E44u    /* "XSND" */
#define SND_DEV_MAX     8              /* 音频设备描述符表项数 */
#define SND_RING_SIZE   1024           /* PCM 环形缓冲字节数 */
#define SND_NAME_MAX    16
#define SND_MIDI_MAX    32             /* MIDI 消息队列深度 */
#define SND_MIX_MAX     4              /* 混音流上限 */

/* 设备类型 */
#define SND_DEV_NONE     0u
#define SND_DEV_PCSPK    1u            /* PC 扬声器（PIT+0x61，真实可发声） */
#define SND_DEV_AC97     2u            /* AC97 控制器（PCI 探测） */
#define SND_DEV_HDA      3u            /* Intel HDA 控制器（PCI 探测） */
#define SND_DEV_USBAUD   4u            /* USB 音频（端点模型，自研） */
#define SND_DEV_BT       5u            /* 蓝牙音频（A2DP/SBC 概要，自研） */

/* 设备状态 */
#define SND_ST_IDLE      0u
#define SND_ST_PLAY      1u
#define SND_ST_REC       2u
#define SND_ST_SUSPEND   3u            /* 省电挂起 */
#define SND_ST_FAULT     4u            /* 故障待恢复 */

/* 打开方式 */
#define SND_O_PLAY       0x01u
#define SND_O_REC        0x02u
#define SND_O_EXCL       0x04u

/* 错误码 */
#define SND_OK           0
#define SND_ENODEV       (-1)
#define SND_EINVAL       (-2)
#define SND_EBUSY        (-3)
#define SND_EFULL        (-4)
#define SND_EMPTY        (-5)
#define SND_EPERM        (-6)
#define SND_ESUSPEND     (-7)

/* 采样率（Hz） */
#define SND_RATE_22050   22050u
#define SND_RATE_44100   44100u
#define SND_RATE_48000   48000u

/* 声道 */
#define SND_CH_MONO      1u
#define SND_CH_STEREO    2u

/* 位深 */
#define SND_BITS_8       8u
#define SND_BITS_16      16u
#define SND_BITS_24      24u
#define SND_BITS_32      32u

/* MIDI 消息 */
#define SND_MIDI_NOTE_ON   0x90u
#define SND_MIDI_NOTE_OFF  0x80u

/* ===== 音频设备描述符 ===== */
typedef struct audio_dev {
    u32  magic;
    u16  type;                 /* SND_DEV_* */
    u16  state;                /* SND_ST_* */
    char name[SND_NAME_MAX];
    u32  rate;                 /* 当前采样率 */
    u16  channels;             /* 1/2 */
    u16  bits;                 /* 8/16/24/32 */
    u16  volume;               /* 0..100 */
    u16  mute;                 /* 0=有声 1=静音 */
    u16  pan;                  /* 0..100：0=全左 50=中 100=全右 */
    u16  flags;                /* 打开方式占用位 */
    u16  refs;                 /* 打开引用计数 */
    u32  frames_played;        /* 累计播放帧数 */
    u32  frames_rec;           /* 累计录制帧数 */
    u32  drop_count;           /* 丢帧计数 */
    u32  underrun_count;       /* 下溢计数 */
    u32  err_count;            /* 错误计数 */
    u32  suspend_count;        /* 挂起次数 */
    u16  vendor;               /* PCI vendor（可探测时） */
    u16  device;               /* PCI device */
    u16  pci_bus, pci_dev, pci_fn;
    u16  present;              /* 1=硬件存在 */
    /* V2 扩展 */
    u16  usb_ep_in;            /* USB 等时端点 IN（录音） */
    u16  usb_ep_out;           /* USB 等时端点 OUT（播放） */
    u16  usb_alt;              /* USB 备用接口 */
    u16  bt_codec;             /* 蓝牙 SBC 编解码器选择 */
    u16  bt_link;              /* 蓝牙链路状态 0=断开 1=连接 2=流式 */
    u16  latency_mode;         /* 0=标准 1=低延迟 */
    u16  underrun_events;      /* 欠载事件计数 */
    u16  overrun_events;       /* 过载事件计数 */
} audio_dev_t;

/* ===== PCM 环形缓冲 ===== */
typedef struct audio_ring {
    u8   buf[SND_RING_SIZE];
    u16  head;
    u16  tail;
    u16  count;
    u16  watermark;            /* 高水位（近满时防溢出保护） */
    u32  overflow;             /* 溢出次数 */
    u32  underrun;             /* 下溢次数 */
} audio_ring_t;

/* ===== PCM 流 ===== */
typedef struct audio_pcm {
    u16  dev_idx;              /* 设备表索引 */
    u16  rate_in;
    u16  ch_in;
    u16  rate_out;
    u16  ch_out;
    u16  playing;              /* 1=播放中 */
    u16  recording;
    u32  frames;               /* 已处理帧数 */
    audio_ring_t ring;
} audio_pcm_t;

/* ===== 路由表：播放设备顺序 ===== */
typedef struct audio_route {
    u16  dev_idx;
    u16  active;               /* 1=在路由中 */
} audio_route_t;

/* ===== 音效参数（均衡 3 段 + 混响） ===== */
typedef struct audio_dsp {
    i16  eq_low_gain;          /* 低频增益 -12..+12 dB */
    i16  eq_mid_gain;
    i16  eq_high_gain;
    u16  eq_active;            /* 均衡使能 */
    u16  rev_delay;            /* 混响延迟样本数（1..256） */
    u16  rev_feedback;         /* 混响反馈 0..90（%） */
    u16  rev_active;           /* 混响使能 */
} audio_dsp_t;

/* ===== MIDI 消息队列 ===== */
typedef struct midi_msg {
    u8   status;               /* 0x90/0x80 */
    u8   note;                 /* 0..127 */
    u8   vel;                  /* 0..127 */
} midi_msg_t;

/* ===== 全局音频子系统状态 ===== */
typedef struct audio_sys {
    u32  magic;
    u32  dev_count;            /* 已注册设备数 */
    u32  pcm_count;            /* 已打开流数 */
    u32  probe_hits;           /* PCI 探测命中数 */
    u32  route_count;
    audio_route_t route[SND_DEV_MAX];  /* 播放路由表 */
    u32  total_frames;         /* 系统累计帧 */
    u32  err_total;
    u16  pcspeaker_on;         /* PC 扬声器当前发声 */
    audio_dev_t dev[SND_DEV_MAX];
    audio_ring_t sysring;      /* 系统级缓冲（时钟/诊断用） */
    /* V2 扩展 */
    audio_dsp_t dsp;           /* 全局音效参数 */
    midi_msg_t midi[SND_MIDI_MAX];  /* MIDI 消息队列 */
    u16  midi_head, midi_tail, midi_count;
    u16  latency_target;       /* 目标低延迟阈值（水位百分比） */
    u16  rec_active;           /* 全局录音活动标记 */
} audio_sys_t;

/* ===== API ===== */
void     sound_init(void);
int      sound_probe_pci(void);              /* 扫描 PCI 找 AC97/HDA 声卡 */
int      sound_register(u16 type, const char *name);
int      sound_dev_open(u16 idx, u16 mode);
int      sound_dev_close(u16 idx);
int      sound_dev_set_rate(u16 idx, u32 rate, u16 ch, u16 bits);
int      sound_volume_set(u16 idx, u16 vol);
int      sound_mute_set(u16 idx, u16 mute);
int      sound_pan_set(u16 idx, u16 pan);    /* V2：声道平衡 */

/* PC 扬声器：PIT 通道 2 频率 + 0x61 门控，真实发声 */
int      sound_tone(u16 freq_hz, u32 ms);
int      sound_beep(void);
int      sound_silence(void);

/* PCM 环形缓冲 */
int      sound_ring_push(audio_ring_t *r, u8 v);
int      sound_ring_pop(audio_ring_t *r, u8 *out);
u16      sound_ring_count(const audio_ring_t *r);

/* 格式/声道/音量处理（16 位有符号样本） */
i16      sound_conv_48k_to_22k(i16 a, i16 b);
i16      sound_mix_stereo_to_mono(i16 l, i16 r);
i16      sound_apply_volume(i16 s, u16 vol); /* 0..100 */
i16      sound_apply_mute(i16 s, u16 mute);
i16      sound_apply_pan(i16 l, i16 r, u16 pan); /* V2：平衡 */

/* V2：录音 / 混音 / 音效 / MIDI / USB / 蓝牙 / 延迟 / 恢复 / 校准 */
int      sound_rec_start(u16 idx);           /* 打开录音路径 */
int      sound_rec_stop(u16 idx);
int      sound_rec_capture(u16 idx, i16 *mono); /* 捕获一帧（模拟） */
int      sound_mix_add(i16 *dst, const i16 *src, u32 n, u16 gain); /* 多流混音 */
int      sound_eq_set(i16 low, i16 mid, i16 high, u16 on);
i16      sound_eq_apply(i16 s, u32 i);        /* 第 i 帧均衡处理 */
int      sound_reverb_set(u16 delay, u16 feedback, u16 on);
i16      sound_reverb_apply(i16 s, u32 i);
int      sound_midi_send(u8 status, u8 note, u8 vel);
int      sound_midi_poll(midi_msg_t *out);
u32      sound_note_freq(u8 note);           /* 12-TET 音符频率表 */
int      sound_usb_register(u16 ep_in, u16 ep_out, u16 alt);
int      sound_bt_register(u16 codec, u16 link);
int      sound_bt_set_sbc(u16 idx, u16 codec, u16 link);
int      sound_latency_set(u16 idx, u16 mode);
int      sound_err_recover(u16 idx);         /* 欠载/过载恢复 */
i16      sound_sine_gen(u32 phase, u32 rate, u32 freq); /* 正弦采样 */
int      sound_calibrate(u16 idx, u16 ref_vol);  /* 音量校准 */

/* 时钟与省电 */
u32      sound_clock_pit_read(void);         /* 读 PIT 通道 0 计数值（真实） */
int      sound_suspend(u16 idx);
int      sound_resume(u16 idx);

/* 路由 / 权限 / 热插拔 */
int      sound_route_add(u16 idx);
int      sound_dev_remove(u16 idx);          /* 热插拔拔出 */

/* 统计与调试 */
void     sound_stats(void);
void     sound_dump(void);

/* 自检（真机可执行） */
int      sound_selftest_core(void);          /* 设备表/探测/打开/参数校验 */
int      sound_selftest_pcm(void);           /* 环形缓冲/水位/溢出/下溢 */
int      sound_selftest_fmt(void);           /* 转换/混音/音量/静音 */
int      sound_selftest_clock(void);         /* PIT 时钟/音调/发声 */
int      sound_selftest_misc(void);          /* 省电/恢复/路由/权限/热插拔/统计 */
int      sound_selftest_dev(void);           /* V2：录音/pan/混音/音效/MIDI/USB/蓝牙/延迟/恢复/校准 */

/* ===== 波形合成器（synth，自研） ===== */
#define SYNTH_SINE     0u
#define SYNTH_SQUARE   1u
#define SYNTH_TRIANGLE 2u
#define SYNTH_SAW      3u
#define SYNTH_NOISE    4u

i16      synth_wave_sample(u8 wave, u32 phase, u16 duty);      /* 单样本合成 */
u16      synth_adsr_gain(u32 t, u32 a, u32 d, u32 s, u32 r, u32 total);
int      synth_render(i16 *buf, u32 samples, u8 wave, u32 freq, u32 rate,
                      u16 amp, u16 duty);                       /* PCM 渲染 */
int      synth_render_adsr(i16 *buf, u32 samples, u8 wave, u32 freq, u32 rate,
                           u16 amp, u16 duty, u32 a, u32 d, u32 s, u32 r);
int      sound_melody_play(const u8 *notes, u32 len, u32 step_ms); /* MIDI 旋律播放 */
int      sound_boot_chime(void);                                  /* 开机提示音 */

#endif /* XOS_SOUND_H */
