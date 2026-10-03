# Basic Bluetooth Audio Example

- [中文版](./README_CN.md)

- Regular Example: ⭐⭐

> ## Project-specific notes (V4 -> ESP32 -> headphones)
>
> **Working rule for this project: always read the official GMF examples first,
> then write code.** They are the tested reference. Local copies:
>
> - `D:\Coding\ESP-IDF\.espressif\esp-gmf-v1.0\gmf_examples\basic_examples\` -
>   among them `pipeline_play_sdcard_music` (file playback), `pipeline_loop_play_no_gap`
>   (end of file, next file, task strategy) and `pipeline_play_multi_source_music`
>   (several sources, close to the mixing we still want to build)
> - `...\packages\esp_bt_audio\examples\bt_audio\` - the example this project was copied from
> - `...\packages\esp_player\examples\` - player service
> - `.espressif\esp-adf_master\adf_examples\` - ADF (old project, IDF 5.5)
>
> This rule is not decoration: two detours in this project came from skipping it.
> The humming at the end of a file was chased with a hand-built task strategy
> that could never fire (`esp_gmf_task.c:347` asks the strategy only once all
> jobs are done), while the examples solve it by remembering the event in the
> callback and stopping the pipeline *outside* the GMF task. And a claim that the
> encoder does not pass `is_done` on was simply wrong - the opposite is in
> `esp_gmf_audio_enc.c:572` and `:598`. Details and the other working rules are
> in `.tmp\ARBEITSWEISE.md`.
>
> This is a copy of `esp-gmf-v1.0/packages/esp_bt_audio/examples/bt_audio`,
> modified for our ESP32-D0WD-V3 (test board, 8 MB PSRAM at 40 MHz). Everything
> below this box describes the *upstream* example and is partly no longer true
> for this project.
>
> ### What was changed
>
> - **No board manager.** `esp_board_manager`, `esp_boards`, `esp_lvgl_port`,
>   `lvgl` and `esp_jpeg` were removed from `main/idf_component.yml`. The
>   board manager expects generated board tables (`components/gen_bmgr_codes`,
>   produced by `idf.py bmgr -b <board>`), which do not exist for our board;
>   without them the link failed with four undefined `g_esp_board_*` symbols.
>   All board-level code in `main.c`, `cmd_reg.c` and `pool_reg.c` was removed
>   accordingly. `pool_reg.c` therefore registers no `io_codec_dev` endpoint -
>   there is no codec device on this board, only `io_file` (SD card) and
>   `io_bt` (Bluetooth stream).
> - **A2DP Source instead of Sink.** The A2DP role is a Kconfig *choice* whose
>   default is sink; `sdkconfig.defaults.esp32` now selects
>   `CONFIG_GMF_EXAMPLE_A2DP_SOURCE`. The ESP32 sends audio to the headphones.
> - **PSRAM 40 MHz instead of 80 MHz** in `sdkconfig.defaults.esp32`. Our
>   module's PSRAM does not run at 80 MHz; the boot hangs in PSRAM init.
> - `main/bt_ui/` (LVGL touch UI, ~21 MB of assets) is kept but **not built**:
>   `CONFIG_EXAMPLE_BT_UI_ENABLE` stays off, and the component cannot be built
>   any more because it needs the board manager for LCD and touch.
> - Volume: this build has no local audio output, so the AVRCP volume from the
>   peer is only tracked and logged in `volume_ctrl_task()` instead of being
>   applied to a codec device.
>
> ### Building on this machine
>
> `idf.py` alone does not work here: `activate.py` / `idf_tools.py export`
> aborts with `ERROR: tool esp-clangd has no installed versions`, because the
> DSH file sandbox denies access to `esp-clangd\...\bin\clangd.exe` and the
> tool's version probe then reports `WinError 2`. Use the wrapper instead:
>
> ```powershell
> powershell -ExecutionPolicy Bypass -File D:\Coding\ESP-IDF\.tmp\idf61_build.ps1 build
> ```
>
> It applies the frozen environment from `.tmp\idf61_env.ps1` (a captured
> `idf_tools.py export --format key-value`) and skips the failing probe.
>
> ### Flashing and driving the CLI
>
> Auto-reset is not wired on our setup (DTR/RTS do not reach EN/GPIO0), and
> GPIO2/DAT0 of the SD bus holds the strapping pin HIGH, so the module has to
> be taken out of the board and put into download mode by hand: hold BOOT, tap
> EN, release BOOT. Idle state for flashing: **module outside the board**.
>
> `idf.py flash` cannot be used for this: it has no `--before`/`--after`
> (they are esptool options), and passing them via `--extra-args` does not
> survive PowerShell argument splitting. The scripts therefore call esptool
> directly, reading the address/file list from `build\flash_args`:
>
> - `.tmp\flash_v4.ps1` - flash and read the serial output
> - `.tmp\read_serial.ps1` - read only, fixed duration
> - `.tmp\send_cmd.ps1` - send CLI commands (`-CommandSequence "a|b|c"`)
> - `.tmp\flash_and_play.ps1` - flash, reset, `connect <mac>`, `start_media`
>
> `--after no-reset` leaves the chip in the bootloader, where it prints
> nothing. A second esptool pass with `--after hard-reset` does **not** help:
> esptool would need the chip in download mode again to talk to it, and it
> answers nothing from the bootloader. Tapping EN/GND by hand is the only
> reliable way, so the scripts wait for it and print a hint.
>
> ### If A2DP connection attempts start failing
>
> Symptom: the headset is found and `connect` is issued, but the link dies in
> service discovery:
>
> ```
> BT_SDP: SDP - Rcvd conn cnf with error: 0x16  CID 0x41
> A2DP connection state: Disconnected
> ```
>
> Cause: the bonding keys in the headset and in the ESP32 have drifted apart
> after repeated firmware changes. The headset tries to reconnect with a key the
> ESP32 no longer knows. Fix: erase the flash (`esptool erase-flash`, which also
> wipes NVS), flash again and pair from scratch -
> `.tmp\erase_flash.ps1` does it with retries.
>
> ### State on hardware (2026-09-26)
>
> Working: IDF v6.1 boot with 8 MB PSRAM at 40 MHz, GMF pool, A2DP **source**
> discovery, connection to a headset and media start:
>
> ```
> A2DP SBC: 44100 Hz, ch_mode 2, bitpool 53, frame 118 bytes
> A2DP connection state: Connected, addr[40:58:99:5e:ee:4f]   (Logitech G435)
> ```
>
> Open: local playback cannot work while the module is outside the board,
> because that also disconnects the microSD socket:
>
> ```
> ESP_GMF_FILE: Failed to open on read, path: /sdcard/test2.mp3,
>               err: No such file or directory
> ```
>
> `main/sd_card.c` mounts 1-bit SDMMC on the fixed ESP32 slot-1 pins
> (CLK=GPIO14, CMD=GPIO15, D0=GPIO2) with card detect on GPIO34. The mount runs
> at boot but is **not** fatal, so Bluetooth still works without a card.
> Verified on hardware with the module in the board: the card mounts and
> `sd_ls` reports `73 entries in /sdcard`, listing `test.mp3` and `test2.mp3`.
>
> Because the module has to be taken out of the board for every flash, the card
> can be attached later without flashing again - three console commands:
>
> ```
> sd_mount      mount /sdcard now (also prints the card-detect state)
> sd_unmount    unmount it
> sd_ls [path]  list a directory, default /sdcard
> ```
>
> There is no card-detect pull-up on this board, so only the "card inserted"
> answer of GPIO34 can be trusted; "no card" may just be a floating pin.
>
> ### Known problem: SD init fails intermittently (0x107)
>
> Symptom, right at boot before anything else touches the card:
>
> ```
> I (1766) SD_HOST: src_freq_hz: 160000000
> E (1795) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
> E (1795) vfs_fat_sdmmc: sdmmc_card_init failed (0x107).
> E (1801) SD_CARD: mount failed: ESP_ERR_TIMEOUT (keine Karte gesteckt?)
> ```
>
> What this is **not**: the card socket switch. On the old project the same
> failure was logged together with `sd_card=1`, i.e. the holder did report a
> card while the bus timed out. It is also not a missing card - the identical
> boot on the same hardware mounted fine minutes earlier (`73 entries in
> /sdcard`).
>
> `0x107` is `ESP_ERR_TIMEOUT` from CMD1/ACMD41 (the OCR handshake), so the card
> never answers at all. The old project (ESP-IDF v5.5.4) had the same
> intermittent failure - there are boot logs of it both succeeding and failing -
> but it survives most boots, while on IDF v6.1 it failed on the observed ones.
>
> Reason found: in IDF v6.1 the old `sdmmc_host_*` API is only a thin wrapper.
> `legacy/src/sdmmc_host.c` calls `sd_host_create_sdmmc_controller()` and
> `sd_host_sdmmc_controller_add_slot()` from `src/sd_host_sdmmc.c`, i.e. the
> rewritten host driver - the `src_freq_hz` log line comes from there. The
> proven 5.5.4 driver is no longer reachable through the public API, and both
> source files are always compiled (`CMakeLists.txt`), so there is no Kconfig
> switch to go back.
>
> Mitigation in `main/sd_card.c`: the mount now retries `SD_MOUNT_ATTEMPTS` (3)
> times with a short delay and logs the card-detect state per attempt. A retry
> was already seen to succeed on this hardware. Beyond that, `sd_mount` on the
> console repeats the whole attempt at runtime without a reboot.
>
> ### Fixed: humming after the end of a file
>
> Symptom was: playback works, and when the file ends a quiet hum continues
> forever over A2DP. **Verified fixed on hardware** - the hum is gone.
>
> Log at the end of a file (this is the state that works):
>
> ```
> I (40340) ESP_GMF_FILE: No more data, ret: 0
> I (40362) ESP_GMF_TASK: Job is done, [... job:...-aud_dec_proc]
> I (40364) ESP_GMF_TASK: Job is done, [... job:...-aud_asrc_proc]
> W (41522) STREAM_PROC: Datei zu Ende gelesen (75531 von 75531 Byte), aber kein FINISHED - stoppe die Wiedergabe
> I (41674) STREAM_PROC: Wiedergabe beendet - stoppe die Pipeline und die A2DP-Uebertragung
> I (41677) ESP_GMF_FILE: CLose, 0x3f804f6c, pos = 75531/75531
> I (41706) STREAM_PROC: [a2dp source pipeline] state => STOPPED(5)
> ```
>
> **Solution: follow the official examples.** `gmf_examples/basic_examples/`
> contains `pipeline_play_sdcard_music` and `pipeline_loop_play_no_gap`, both of
> which remember the pipeline event in the callback and call
> `esp_gmf_pipeline_stop()` from their own flow afterwards - never from inside
> the callback, which runs on the GMF task and would wait on itself:
>
> ```c
> if ((event->sub == ESP_GMF_EVENT_STATE_STOPPED) ||
>     (event->sub == ESP_GMF_EVENT_STATE_FINISHED) ||
>     (event->sub == ESP_GMF_EVENT_STATE_ERROR)) {
>     xEventGroupSetBits(ctx, PIPELINE_BLOCK_BIT);
> }
> ...
> esp_gmf_pipeline_stop(pipe);
> ```
>
> Implemented the same way in `stream_proc.c`: the callback only calls
> `local2bt_request_stop()`, and `stream_proc_task` performs the stop.
>
> Two wrong turns are worth remembering, because both came from not reading the
> examples and the component sources first:
>
> 1. A hand-built task strategy (`esp_gmf_task_set_strategy_func` with
>    `GMF_TASK_STRATEGY_ACTION_STOP`) **cannot work here**:
>    `esp_gmf_task.c:347` asks the strategy only once *all* jobs are done, and
>    the log line `Finish, strategy action:` was never printed.
> 2. The claim that the encoder does not pass `is_done` on was **wrong** -
>    `esp_gmf_audio_enc.c:572` and `:598` return `ESP_GMF_JOB_ERR_DONE` when
>    `in_load->is_done` is set. The encoder job does finish.
>
> **Still open:** `ESP_GMF_EVENT_STATE_FINISHED` never arrives even though all
> three element jobs (`aud_dec_proc`, `aud_asrc_proc`, `aud_enc_proc`) report
> done. So the event path stays unused and the fallback does the work. The
> fallback polls the file position (`esp_gmf_io_get_pos`) and stops once the
> position stops moving **and** has reached the size set via
> `esp_gmf_io_set_size` - the size is filled in from `stat()` because GMF's file
> IO never sets `attr.size` itself. The `pos >= size` condition matters: without
> it a brief stall mid-file (a stuttering A2DP link) would abort playback.
>
> ### Fixed: playlist held WAV files
>
> The playlist still listed `test_tone_440.wav` and `test_tone_48k.wav`, which
> the pipeline's MP3 decoder (`esp_gmf_audio_dec_reconfig`, type
> `ESP_AUDIO_TYPE_MP3`) cannot read. It is now `test2.mp3` and `test.mp3`.

> ### I2S input from the Vampire - findings (Schritt 2)
>
> Checked as required: the official examples first, then the component sources.
>
> **Does the I2S API change in IDF v6.1?** The standard-mode API did not change.
> `driver/i2s_std.h` exposes exactly the same functions in v6.1 and v5.5.4
> (`i2s_new_channel`, `i2s_channel_init_std_mode`, `i2s_channel_enable`,
> `i2s_channel_read/write`). What is new in `i2s_common.h`:
>
> - `i2s_chan_config_t` gained `tx_destination` / `rx_destination`
>   (`i2s_destination_t`)
> - three TX-only calls: `i2s_channel_config_tx_fifo_sync`,
>   `i2s_channel_enable_tx_fifo_sync`, `i2s_channel_get_sync_count`
>
> `I2S_DESTINATION_BT` would route I2S data straight to the Bluetooth
> controller, bypassing the CPU - attractive for our job, but it is gated behind
> `SOC_I2S_SUPPORTS_BT_DEST`, which is **not defined for ESP32** (nor for S3/P4),
> so it is not available to us. Nothing to migrate for our RX path.
>
> **Is there a GMF I2S input?** Yes: `esp_gmf_io_i2s_pdm`
> (`managed_components/espressif__gmf_io/`). The name is misleading - it is
> driver-agnostic. `_i2s_pdm_acquire_read()` only calls
> `i2s_channel_read(cfg->pdm_chan, ...)` and `_i2s_pdm_open()` calls
> `i2s_channel_enable()`, so it works with a standard I2S channel as well; the
> channel handle is supplied by the caller. Only the TX path is PDM-specific.
> It registers as `ESP_GMF_IO_TYPE_BYTE`, reader or writer.
>
> **How does mixing work?** The reference is
> `gmf_examples/basic_examples/pipeline_howl/main/pipeline_howl.c`. Note that
> `pipeline_play_multi_source_music` does **not** mix - it switches sources by
> replacing the input IO. `pipeline_howl` builds three pipelines and joins them:
>
> ```c
> const char *music_chain[] = {"aud_dec", "aud_rate_cvt", "aud_bit_cvt", "aud_ch_cvt"};
> const char *mix_chain[]   = {"aud_mixer"};
> esp_gmf_pool_new_pipeline(pool, "io_file", music_chain, ..., NULL,        &pipe_music);
> esp_gmf_pool_new_pipeline(pool, NULL,      mix_chain,   ..., "io_codec_dev", &pipe_mix);
>
> esp_gmf_db_new_ringbuf(10, 1024, &rb_music);
> out_port = NEW_ESP_GMF_PORT_OUT_BYTE(esp_gmf_db_acquire_write, esp_gmf_db_release_write,
>                                      esp_gmf_db_deinit, rb_music, 4096, ESP_GMF_MAX_DELAY);
> in_port  = NEW_ESP_GMF_PORT_IN_BYTE(esp_gmf_db_acquire_read, esp_gmf_db_release_read,
>                                     esp_gmf_db_deinit, rb_music, 4096, 1);
> esp_gmf_pipeline_connect_pipe(pipe_music, "aud_ch_cvt", out_port,
>                               pipe_mix,   "aud_mixer",  in_port);
> ```
>
> Mixer settings are per source: `esp_ae_mixer_cfg_t.src_info[i]` with
> `weight1`, `weight2`, `transit_time`, plus
> `esp_gmf_mixer_set_mode(el, i, ESP_AE_MIXER_MODE_FADE_UPWARD)`.
>
> **The ASRC on ESP32 works in software.** There is no hardware ASRC
> (`CONFIG_SOC_ASRC_SUPPORTED` is off), but `esp_asrc.c:144` falls back to
> `asrc_sw_get_rate_cvt_ops()` and friends, so rate conversion is available. This
> matters because the Vampire runs at **60 kHz / 32 bit / stereo** as I2S master
> (from the old project: `I2S_NUM_0`, `I2S_ROLE_SLAVE`,
> `I2S_DATA_BIT_WIDTH_32BIT`, `I2S_SLOT_MODE_STEREO`, `mclk_multiple 1152`) while
> the SBC encoder is pinned to 44.1 kHz.
>
> **Planned architecture.** Keep the existing SD pipeline, add an I2S pipeline,
> and merge both in a mixer pipeline that feeds the SBC encoder:
>
> ```
> io_file -> aud_dec -> aud_asrc ------------------\
>                                                   >-- aud_mixer -> aud_asrc -> aud_enc -> io_bt
> io_i2s (Vampire, 60k/32/stereo) -> aud_asrc -----/
> ```
>
> The EOF handling described above must exempt the I2S branch: an I2S stream has
> no end, so only the file branch may trigger a stop.
>
> ### Wiring (from the schematic notes)
>
> | ESP32 | Vampire P20 | Signal |
> |---|---|---|
> | GND | 2 | GND |
> | GPIO25 | 6 | WSEL / word select |
> | GPIO5 | 8 | BCLK |
> | GPIO35 | 10 | DIN (data to the ESP32) |
> | GPIO18 | I2C 2 | SDA |
> | GPIO23 | I2C 3 | SCL |
>
> Checked against the rest of the project - no collisions:
>
> - SD card: CLK=14, CMD=15, D0=2, CD=34
> - console UART: GPIO1/GPIO3 (reported in the boot log)
> - GPIO35 is input-only, which is exactly right for DIN
>   (`SOC_GPIO_VALID_OUTPUT_GPIO_MASK` excludes bits 34-39)
> - GPIO5 is an SDIO-timing strapping pin, but the old project drove BCLK on it,
>   so it is proven on this hardware; the strapping effect is about SDIO slave
>   timing and does not affect us
>
> **Data width caveat:** `SOC_I2S_MAX_DATA_WIDTH` is 24 on the ESP32, while the
> old project configured `I2S_DATA_BIT_WIDTH_32BIT` with a 32-bit slot and
> converted down to 16 bit in software. With GMF the chain is
> `aud_bit_cvt` + `aud_asrc`, so a 24-bit slot and a bit conversion to 16 bit is
> the cleaner route; this is the one detail to verify against the real signal.
>
> ### Step 2b implemented: I2S input -> SBC -> Bluetooth
>
> New files:
>
> - `main/i2s_input.c` / `.h` - I2S RX channel (slave, 60 kHz, 32 bit, stereo,
>   BCLK=GPIO5, WS=GPIO25, DIN=GPIO35) wrapped as a GMF IO through
>   `esp_gmf_io_i2s_pdm_init()`. It also starts a task that logs the throughput
>   once per second: at 60 kHz / 32 bit / stereo the expected value is
>   **480000 bytes per second**, so a reading of zero immediately tells us the
>   Vampire is not sending (or the wiring is wrong) - no headset needed to check.
> - `stream_proc.c` - `setup_pipeline_i2s2bt()` plus the request/start/stop
>   functions. The chain is
>   `io_i2s -> aud_asrc -> aud_enc (SBC) -> io_bt`.
>
> CLI:
>
> ```
> connect 40:58:99:5e:ee:4f
> start_media          (or i2s_media first, see below)
> i2s_media            request the I2S input; it starts on the next A2DP stream
> i2s_media off        stop the I2S pipeline
> ```
>
> Order matters: `i2s_media` only sets a flag, because without an A2DP stream
> there is no encoder target. So run `connect`, then `start_media`, then
> `i2s_media` - or `i2s_media` first and then `connect` + `start_media`.
>
> The encoder is configured at runtime with
> `esp_gmf_audio_enc_reconfig_by_sound_info()`; `ESP_AUDIO_TYPE_SBC` is filled
> in there with `ESP_SBC_ENC_CONFIG_DEFAULT()` plus the basic fields from the
> sound info (`esp_gmf_audio_enc.c:448 ff.`). This is needed because, unlike the
> `local2bt` path, there is no A2DP stream at setup time to supply codec
> configuration.
>
> Not done yet (step 2c): the two branches run **separately**, not mixed. Wiring
> them into one `aud_mixer` follows `pipeline_howl` - see the section above.
>
> ### Key constraint found on hardware: one element belongs to one task
>
> Symptom when both branches were started:
>
> ```
> W ESP_GMF_PIPELINE: Element[aud_asrc-0x3f806984] not ready to register job, ret:0xffffdff8
> E ESP_GMF_TASK: Run timeout, [tsk:i2s2bt_task]
> ```
>
> `esp_gmf_pipeline_loading_jobs()` walks the element chain and calls
> `register_working_jobs_to_task()` for each one
> (`esp_gmf_pipeline.c:385`). An element that is already bound to another task
> cannot be bound again, so the second pipeline gets no jobs and its task times
> out.
>
> **A pool element is not per-pipeline state**: `esp_gmf_pool_new_pipeline()`
> duplicates the objects (`esp_gmf_obj_dupl`, `esp_gmf_pool.c:202`), but two
> pipelines built from the same pool entry still share the same instance. Since
> `local2bt` (file playback) and `i2s2bt` both need `aud_asrc` and `aud_enc`,
> whichever runs first wins and the other fails.
>
> The official examples never hit this because **every pipeline has its own
> elements**: in `pipeline_howl`, `pipe_music` has `aud_dec`, `aud_rate_cvt`,
> `aud_bit_cvt`, `aud_ch_cvt`; `pipe_mic` has `aud_howl`; only `aud_mixer` sits
> in `pipe_mix`. In `pipeline_audio_effects` it is the same pattern.
>
> Two changes made:
>
> 1. `pool_reg.c` registers dedicated instances for the I2S branch:
>    `aud_asrc_i2s` and `aud_enc_i2s`, and `setup_pipeline_i2s2bt()` uses those
>    names.
> 2. As long as there is no mixer, the two branches are **mutually exclusive**
>    (`i2s2bt_requested` selects the I2S branch in the ALLOCATED state and the
>    file branch is skipped), so the I2S path can be tested on its own.
>
> For step 2c the structure has to follow the example: a file branch and an I2S
> branch, each with their own converter and no encoder, meeting in a separate
> mixer pipeline that owns the encoder and `io_bt`.
>
> ### Still unverified
>
> The I2S branch has not yet run with data. `i2s_media` is accepted and the
> pipeline is created, but the throughput line reports
>
> ```
> W I2S_INPUT: I2S: keine Daten (gesamt 0 Byte) - sendet die Vampire?
> ```
>
> Note the counter only moves while a pipeline is reading: the speed statistics
> are updated in the data-bus path (`esp_gmf_io.c:512`, `:632`), so a zero
> reading before the pipeline runs means nothing. What has to be checked on
> hardware now is whether the number becomes ~480000 bytes/s once the I2S
> pipeline is actually running.
>
> ### `aud_asrc` does not work in this pipeline
>
> With a dedicated ASRC element for the I2S branch (`aud_asrc_i2s`) the job
> registration still failed:
>
> ```
> W ESP_GMF_PIPELINE: Element[aud_asrc_i2s-0x3f80285c] not ready to register job, ret:0xffffdff8
> E ESP_GMF_TASK: Run timeout, [tsk:i2s2bt_task]
> ```
>
> `0xffffdff8` is `ESP_GMF_ERR_NOT_READY` (`ESP_GMF_ERR_BASE - 8`). The same
> failure happened with the file branch disabled (`-DI2S_ONLY_TEST=ON`), so it is
> **not** caused by two pipelines sharing elements - it is the ASRC element
> itself in this pipeline.
>
> Note the ASRC works fine in the file pipeline (`local2bt`), where it converts
> to 44.1 kHz. Whatever it waits for is specific to how the I2S pipeline is set
> up.
>
> **Workaround in place, following the examples:** the examples do not use an
> ASRC for this at all. `pipeline_howl` and `pipeline_audio_effects` use the
> chain `aud_dec -> aud_rate_cvt -> aud_bit_cvt -> aud_ch_cvt`. The I2S pipeline
> is therefore now
>
> ```
> io_i2s -> aud_rate_cvt_i2s -> aud_bit_cvt_i2s -> aud_enc_i2s -> io_bt
> ```
>
> with `esp_gmf_rate_cvt_set_dest_rate()` and `esp_gmf_bit_cvt_set_dest_bits()`
> (`esp_gmf_bit_cvt.h:33`, `esp_gmf_rate_cvt.h:31`). Both converters are
> registered as dedicated instances in `pool_reg.c`. Not yet hardware-verified.
>
> ### Diagnostic switch
>
> `main/CMakeLists.txt` has an `I2S_ONLY_TEST` option (default OFF). With
> `idf.py -DI2S_ONLY_TEST=ON build` the file branch is not created at all, which
> is how the ASRC problem above was isolated. The file branch has to come back
> once the I2S chain runs - for the mixer in step 2c anyway in a new shape.
>
> ### Four blockers found on the way to a running I2S branch
>
> Audio from the Vampire does play now. Getting there took four separate faults,
> all in our own code, each found by reading the framework sources and the
> examples rather than guessing:
>
> 1. **Two pipelines sharing an element.** A pool element is not per-pipeline
>    state; `local2bt` and `i2s2bt` both used `aud_asrc` and `aud_enc`, and an
>    element can only be bound to one task (`esp_gmf_pipeline.c:385`). Fix:
>    dedicated instances `aud_rate_cvt_i2s`, `aud_bit_cvt_i2s`, `aud_enc_i2s`.
>
> 2. **`dependency = true` elements need sound info.** `aud_rate_cvt` and
>    `aud_bit_cvt` are created with `el_cfg.dependency = true`
>    (`esp_gmf_rate_cvt.c:297`, `esp_gmf_bit_cvt.c:273`), so they start in
>    `STATE_NONE`. They only reach `INITIALIZED` when the sound info arrives
>    (`esp_gmf_rate_cvt.c:186`), and `register_working_jobs_to_task()` refuses
>    anything not `INITIALIZED` with `ESP_GMF_ERR_NOT_READY` (0xffffdff8,
>    `esp_gmf_pipeline.c:55`). Fix: report the source format before running
>
>    ```c
>    esp_gmf_info_sound_t info = { .sample_rates = 60000, .channels = 2, .bits = 32 };
>    esp_gmf_pipeline_report_info(i2s2bt_pipe, ESP_GMF_INFO_SOUND, &info, sizeof(info));
>    ```
>
>    exactly as `pipeline_howl.c:297-303` does.
>
> 3. **I2S IO had a task but no data bus.** `esp_gmf_io_open()` starts an
>    asynchronous IO with its own task as soon as `thread.stack > 0`, and that
>    path *requires* `buffer_cfg.buffer_size > 0`, otherwise it fails with
>    "Failed to create data bus" (`esp_gmf_io.c:327-350`). Fix: set both,
>    `io_size = 2048` (one read) and `buffer_size = 32 * 1024` (the bus). At
>    480000 bytes/s the bus then covers ~68 ms instead of ~17 ms.
>
> 4. **A diagnostic that cost audio quality.** The throughput monitor read
>    `esp_gmf_io_get_speed_stats()` on the *pool* IO, while the pipeline works
>    with a clone - so it always read 0 and, worse, wrote a warning every second.
>    At 115200 baud each line blocks the UART for ~9 ms. Removed; the audio
>    itself is the better measure. Do not bring it back in that form. (The
>    speed counter is updated in the data-bus path, `esp_gmf_io.c` line 512 and
>    632, and the clone's driver calls do count - but only on the instance the
>    pipeline actually uses.)
>
> ### Audio path works
>
> Confirmed by ear: the Vampire's sound arrives at the headset, and pausing the
> Vampire silences it - so the I2S branch really is live, not a stale buffer.
>
> ### The choppy playback: wrong SBC parameters, not buffering
>
> Choppy audio was **not** a buffer or CPU problem. The log said so all along:
>
> ```
> W BT_AUD_A2D_SRC: Drop oversized frame batch: 1340 bytes exceeds MTU 666
> ```
>
> Every send block was discarded. 5 frames x 268 bytes = 1340 bytes, and the
> MTU is 666. File playback produces 118-byte frames (5 x 118 = 590, fits), and
> that is the whole difference.
>
> Cause: the I2S branch configured its encoder with
> `esp_gmf_audio_enc_reconfig_by_sound_info()`. For stereo that path sets
>
> ```c
> /* esp_gmf_audio_enc.c:452 ff. */
> } else if (info->channels == 2) {
>     sbc_enc_cfg.ch_mode = ESP_SBC_CH_MODE_DUAL;
> ```
>
> and leaves `bitpool` at its default - so the frames came out at 268 bytes.
> The negotiated parameters (bitpool from min/max, block length, sub-bands,
> joint stereo) were available the whole time: they arrive with the stream as
> `codec_info.codec_cfg` (24 bytes, see the `Codec Info:` log line), and the
> file branch already hands them to `esp_gmf_audio_enc_reconfig()`.
>
> Fix in `i2s2bt_set_stream()`: set the type first, then apply the negotiated
> configuration:
>
> ```c
> esp_gmf_info_sound_t enc_info = { .format_id = ESP_AUDIO_TYPE_SBC, ... };
> esp_gmf_audio_enc_reconfig_by_sound_info(enc, &enc_info);   /* type */
> esp_audio_enc_config_t enc_cfg = {
>     .type = ESP_AUDIO_TYPE_SBC,
>     .cfg  = codec_info.codec_cfg,
>     .cfg_sz = codec_info.cfg_size,
> };
> esp_gmf_audio_enc_reconfig(enc, &enc_cfg);                  /* parameters */
> ```
>
> Result on hardware: `acquire in frame: 512, out frame: 118`, and zero
> `Drop oversized` lines.
>
> **Lesson:** the answer was in the log from the first I2S run. Reading a log
> only superficially - the way it was done here - cost several flash cycles.

## Example Brief

This example initializes Bluetooth audio through the `esp_bt_audio` module and uses `esp_gmf_io_bt` to link the Bluetooth audio stream with a GMF pipeline, enabling Classic Bluetooth playback/uplink streaming and LE Audio TMAP unicast or broadcast-receiver flows when the target and Bluetooth configuration support them. It also provides serial commands to demonstrate control of Bluetooth audio playback, LE discovery/connection, and voice calls. When `CONFIG_EXAMPLE_BT_UI_ENABLE` is enabled on a board with LCD and touch support, the example additionally provides an on-device LVGL touch-screen UI with a media player, dialer, and volume bar.

### Typical Scenarios

- **Bluetooth speaker (A2DP Sink)**: Phone connects to the device to play music; supports play/pause/next/previous, volume, and metadata
- **Bluetooth source (A2DP Source)**: Device discovers and connects to Bluetooth headphones or speakers and streams local or microSD audio to the remote device
- **Bluetooth voice call (HFP HF)**: Answer/reject incoming calls, dial; call state and telephony status reporting; AEC in the GMF pipeline to improve call clarity; fetch phonebook and call history
- **LE Audio speaker/headset (TMAP)**: Device exposes LE Audio sink/source capabilities for unicast media or conversational audio, and can optionally act as a broadcast media receiver
- **On-device UI (optional)**: LVGL touch-screen UI with splash screen, media player (cover art, track info, playback controls), dialer (numeric keypad with call button), and auto-hiding volume bar

### Prerequisites

- This example involves Bluetooth concepts and protocols; see the official [Bluetooth Specifications](https://www.bluetooth.com/specifications/specs/)
- This example uses `esp_board_manager` for board-level resources; see [ESP Board Manager](https://github.com/espressif/esp-board-manager) for setup

### Resources

- An audio development board with Audio DAC/ADC, I2S, and microSD (e.g. lyrat_mini_v1_1); for A2DP Source, prepare a microSD card and test audio files

## Environment Setup

### Hardware Required

- **Board**: Default Classic Bluetooth setup is `lyrat_mini_v1_1`; for LE Audio, use an ESP target and controller configuration that support BLE ISO/Bluetooth Audio, plus an audio board with I2S codec resources
- **Peripherals**: Audio DAC, Audio ADC, I2S, microSD card (for A2DP Source, store `media0.mp3`, `media1.mp3`, `media2.mp3`), LCD and touch panel (required for the optional UI)
- **Bluetooth**: Classic Bluetooth (BR/EDR) for A2DP, AVRCP, and HFP; LE Audio requires NimBLE, Bluetooth Audio, and ISO support

### Default IDF Branch

This example supports IDF release/v5.5 (>= v5.5.2).

### Software Requirements

- For A2DP Source, place three test audio files on the microSD root: `media0.mp3`, `media1.mp3`, `media2.mp3`
- For A2DP Sink, a phone or other A2DP Source device is needed; for A2DP Source, Bluetooth headphones or a speaker are needed
- For LE Audio, enable `CONFIG_BT_NIMBLE_ENABLED`, `CONFIG_BT_AUDIO`, `CONFIG_BT_ISO`, and the required ESP-IDF LE Audio profile options; use an LE Audio peer or broadcast receiver/source that matches the selected role

## Build and Flash

### Build Preparation

Before building this example, ensure the ESP-IDF environment is set up. If it is already set up, skip to the project directory and run the board setup steps below. If not, run the following in the ESP-IDF root directory to complete the environment setup. For full steps, see the [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/index.html).

```
./install.sh
. ./export.sh
```

Short steps:

- Go to this example's project directory (example path below; replace with your actual path):

```
cd $YOUR_GMF_PATH/packages/esp_bt_audio/examples/bt_audio
```

- This example uses `esp_board_manager` for board-level resources; add board support first.

> ESP-IDF supports adding a specific sdkconfig defaults file through `SDKCONFIG_DEFAULTS`. On S31, select the Classic or LE Audio defaults before running `idf.py set-target` / build commands:
>
> ```text
> export SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s31.classic
> # or:
> export SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s31.le
> # PowerShell:
> $env:SDKCONFIG_DEFAULTS = "sdkconfig.defaults.esp32s31.classic"
> ```

This example uses [ESP Board Manager](https://github.com/espressif/esp-board-manager) to manage board-level resources. The [`esp-bmgr-assist`](https://pypi.org/project/esp-bmgr-assist/) helper tool is recommended as the default entry point.

Install once in your activated ESP-IDF Python environment:

```bash
pip install esp-bmgr-assist
pip install --upgrade esp-bmgr-assist
```

- List supported boards:

```bash
idf.py bmgr -l
```

Example output:

```text
ℹ️  Main Boards:
  [1] dual_eyes_board_v1_0
  [2] esp32_c3_lyra
  [3] esp32_c5_spot
  [4] esp32_p4_function_ev
  [5] esp32_s3_korvo2_v3
  [6] esp32_s3_korvo2l
  [7] esp_box_3
  [8] esp_box_lite
  [9] esp_hi
```

- Select a board:

```bash
idf.py bmgr -b <board_index|board_name>
```

For example, to select `esp32_s3_korvo2_v3`:

```bash
idf.py bmgr -b 5
# or
idf.py bmgr -b esp32_s3_korvo2_v3
```

On first invocation, the component is downloaded automatically based on the `espressif/esp_board_manager` dependency declared in `main/idf_component.yml`.

> [!NOTE]
> To switch to a different board supported by `esp_board_manager`, repeat the same steps with the new board name or index.
> For a custom board, see [How to customize board](https://github.com/espressif/esp-board-manager/blob/main/esp_board_manager/docs/how_to_customize_board.md).
> For more information about `esp_board_manager`, see the [ESP Board Manager Getting Started Guide](https://github.com/espressif/esp-board-manager/blob/main/esp_board_manager/README.md).

### Project Configuration

Select the Bluetooth role and options in menuconfig:

```bash
idf.py menuconfig
```

Configure the following in menuconfig (example):

- `BT Audio Basic Example (GMF)` → `Classic Audio Roles Configuration` → Select A2DP role (A2DP Sink / A2DP Source) or HFP HF, etc
- `BT Audio Basic Example (GMF)` → `Enable LE Audio` → enable the LE Audio example flow when the target supports it
- Optional: `BT UI Configuration` → `Enable LVGL UI` → enable the on-device touch-screen UI (requires LCD and touch panel)
- Optional: `LE Audio Configuration` → select the LE Audio user case and TMAP roles, then configure LE audio location, source capability, and coordinated set size
- For A2DP Source, ensure microSD is configured and the card contains `media0.mp3`, `media1.mp3`, `media2.mp3`

> Press `s` to save and `Esc` to exit after configuration.

### Build and Flash Commands

- Build the example:

```
idf.py build
```

- Flash the firmware and run the serial monitor (replace PORT with your port name):

```
idf.py -p PORT flash monitor
```

Exit the monitor with `Ctrl-]`.

## How to Use the Example

### Functionality and Usage

- **Roles and commands**: The example supports Classic Bluetooth roles (A2DP Sink, A2DP Source, HFP HF, AVRCP Controller/Target) and LE Audio TMAP presets selectable via menuconfig; after building and flashing, type `help` in the serial console to see the command list
- **A2DP Sink**: The device waits for a phone or other source to connect; after connection, use serial commands to control playback: `play`, `pause`, `stop`, `next`, `prev`, and `vol_set <0-100>` for volume
- **A2DP Source**: Use `start_discovery` and `connect <mac>` to discover and connect to a Bluetooth speaker or headphones; use `start_media` and `stop_media` to control streaming
- **HFP HF**: Supports answer/reject incoming call, dial, and call/telephony status reporting; the AEC element in the GMF pipeline is used for echo cancellation during calls
- **PBAP Client**: Use the `pb_fetch` command to retrieve the phonebook and call history
- **LE Audio**: Use `le_scan_start [timeout_ms]` and `le_scan_stop` to discover LE Audio devices, `le_connect <addr_type> <mac_address> [timeout_ms]` to connect to a peer, and `le_disconnect` to disconnect the current LE ACL link. Media, volume, call, and stream events are reported through the same `esp_bt_audio` event path
- **LVGL UI** (when `CONFIG_EXAMPLE_BT_UI_ENABLE=y`): The on-device touch-screen UI shows a splash screen before Bluetooth connection and switches to a tab-based main screen on connection with a **media player** (cover art display, track title/artist, play/pause/prev/next controls, stream-type indicator for CIS/BIS) and a **dialer** (numeric keypad, call start/end, incoming/active call display). An auto-hiding **volume bar** appears on volume-change events
- **Companion devices**: A2DP Sink needs a phone or other A2DP Source; A2DP Source needs Bluetooth headphones or a speaker; HFP needs a phone that supports HFP AG; LE Audio needs a compatible LE Audio peer or broadcast device

### Log Output

The following is a sample of key log lines during startup (board and GMF init, Bluetooth and pipeline ready):

```c
I (1398) main_task: Calling app_main()
I (1423) PERIPH_I2C: I2C master bus initialized successfully
W (1425) PERIPH_I2S: I2S[0] STD already enabled, tx:0x3f800dd8, rx:0x3f800f94
I (1425) PERIPH_I2S: I2S[0] STD,  TX, ws: 25, bclk: 5, dout: 26, din: 35
I (1431) PERIPH_I2S: I2S[0] initialize success: 0x3f800dd8
I (1437) PERIPH_I2S: I2S[1] STD, RX, ws: 33, bclk: 32, dout: -1, din: 36
I (1443) PERIPH_I2S: I2S[1] initialize success: 0x3f80136c
I (1448) PERIPH_GPIO: Initialize success, pin: 13, set the default level: 1
I (1455) PERIPH_GPIO: Initialize success, pin: 19, default_level: 0
I (1461) PERIPH_GPIO: Initialize success, pin: 21, set the default level: 0
I (1467) PERIPH_GPIO: Initialize success, pin: 22, set the default level: 0
I (1474) PERIPH_GPIO: Initialize success, pin: 27, set the default level: 0
I (1481) PERIPH_GPIO: Initialize success, pin: 34, default_level: 0
I (1490) PERIPH_ADC: Create adc oneshot unit success
I (1491) BOARD_MANAGER: All peripherals initialized
I (1496) DEV_POWER_CTRL_SUB_GPIO: Initializing GPIO power control: gpio_sd_power
I (1503) BOARD_PERIPH: Reuse periph: gpio_sd_power, ref_count=2
I (1509) DEV_POWER_CTRL_SUB_GPIO: GPIO power control initialized successfully
I (1516) DEV_POWER_CTRL: Power control device initialized successfully, sub_type: gpio
I (1523) BOARD_PERIPH: Reuse periph: i2s_audio_out, ref_count=2
I (1529) DEV_AUDIO_CODEC: DAC is ENABLED
I (1533) DEV_AUDIO_CODEC: Init audio_dac, i2s_name: i2s_audio_out, i2s_rx_handle:0x0, i2s_tx_handle:0x3f800dd8, data_if: 0x3ffd66c4
I (1544) BOARD_PERIPH: Reuse periph: i2c_master, ref_count=2
I (1558) ES8311: Work in Slave mode
I (1561) DEV_AUDIO_CODEC: Successfully initialized codec: audio_dac
I (1562) DEV_AUDIO_CODEC: Create esp_codec_dev success, dev:0x3ffd6844, chip:es8311
I (1570) DEV_AUDIO_CODEC: ADC is ENABLED
I (1573) BOARD_PERIPH: Reuse periph: i2s_audio_in, ref_count=2
I (1579) DEV_AUDIO_CODEC: Init audio_adc, i2s_name: i2s_audio_in, i2s_rx_handle:0x3f80136c, i2s_tx_handle:0x3f8011b0, data_if: 0x3ffd688c
I (1591) BOARD_PERIPH: Reuse periph: i2c_master, ref_count=3
I (1613) DEV_AUDIO_CODEC: Successfully initialized codec: audio_adc
I (1613) DEV_AUDIO_CODEC: Create esp_codec_dev success, dev:0x3ffd69c0, chip:es7243e
I (1615) BOARD_DEVICE: Device sdcard_power_ctrl config found: 0x3f433fb8 (size: 20)
I (1623) DEV_POWER_CTRL_SUB_GPIO: GPIO power control: ON, level: 0 for device: fs_sdcard
I (1630) DEV_FS_FAT_SUB_SDMMC: slot_config: cd=-1, wp=-1, clk=14, cmd=15, d0=2, d1=-1, d2=-1, d3=-1, d4=-1, d5=-1, d6=-1, d7=-1, width=1, flags=0x1
Name: BB1QT
Type: SDHC
Speed: 40.00 MHz (limit: 40.00 MHz)
Size: 30528MB
CSD: ver=2, sector_size=512, capacity=62521344 read_bl_len=9
SSR: bus_width=1
I (1851) DEV_FS_FAT: Filesystem mounted, base path: /sdcard
I (1857) BOARD_PERIPH: Reuse periph: adc_button, ref_count=2
I (1862) BOARD_PERIPH: Peripheral adc_button config found: 0x3f43410c (size: 52)
I (1869) DEV_BUTTON_SUB_ADC: Initializing 6 ADC buttons on unit 0, channel 3
I (1876) adc_button: ADC1 has been initialized
I (1880) adc_button: calibration scheme version is Line Fitting
I (1886) adc_button: Calibration Success
I (1889) button: IoT Button Version: 4.1.6
I (1893) DEV_BUTTON: Successfully initialized button: adc_button_group, sub_type: adc_multi
I (1901) BOARD_MANAGER: Board manager initialized
I (1906) BOARD_DEVICE: Device handle audio_dac found, Handle: 0x3ffd669c TO: 0x3ffd669c
I (1914) I2S_IF: channel mode 0 bits:16/16 channel:2 mask:3
I (1919) I2S_IF: STD Mode 1 bits:16/16 channel:2 sample_rate:48000 mask:3
I (1942) Adev_Codec: Open codec device OK
I (1942) BOARD_DEVICE: Device handle audio_adc found, Handle: 0x3ffd6874 TO: 0x3ffd6874
I (1943) I2S_IF: channel mode 0 bits:16/16 channel:2 mask:3
I (1948) I2S_IF: STD Mode 0 bits:16/16 channel:2 sample_rate:48000 mask:3
I (1954) I2S_IF: channel mode 0 bits:16/16 channel:2 mask:3
I (1959) I2S_IF: STD Mode 1 bits:16/16 channel:2 sample_rate:48000 mask:3
I (1967) Adev_Codec: Open codec device OK
I (1970) POOL_INIT: Registering GMF pool
I (1975) POOL_INIT: Registered: aud_aec
I (1977) BOARD_DEVICE: Device handle audio_dac found, Handle: 0x3ffd669c TO: 0x3ffd669c
I (1985) BOARD_DEVICE: Device handle audio_adc found, Handle: 0x3ffd6874 TO: 0x3ffd6874
I (1993) POOL_INIT: GMF pool initialization completed successfully
W (1999) ESP_GMF_THREAD: Make sure selected the `CONFIG_SPIRAM_BOOT_INIT` and `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` by `make menuconfig`
I (2011) ESP_GMF_TASK: Waiting to run... [tsk:bt2codec_task-0x3ffd79bc, wk:0x0, run:0]
W (2019) ESP_GMF_THREAD: Make sure selected the `CONFIG_SPIRAM_BOOT_INIT` and `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` by `make menuconfig`
I (2031) ESP_GMF_TASK: Waiting to run... [tsk:codec2bt_task-0x3ffd92c4, wk:0x0, run:0]
W (2032) ESP_GMF_THREAD: Make sure selected the `CONFIG_SPIRAM_BOOT_INIT` and `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` by `make menuconfig`
I (2051) ESP_GMF_TASK: Waiting to run... [tsk:local2bt_task-0x3ffdab74, wk:0x0, run:0]
I (2051) BTDM_INIT: BT controller compile version [045a658]
I (2064) BTDM_INIT: Using main XTAL as clock source
I (2069) BTDM_INIT: Bluetooth MAC: a8:42:e3:66:1f:da
I (2075) phy_init: phy_version 4863,a3a4459,Oct 28 2025,14:30:06
I (2564) BT_AUD_HOST: Setting bluedroid discovery operations
I (2566) BT_AUD_AVRC_CT: CT init success
I (2568) BT_AUD_AVRC_TG: TG init success
W (2571) BT_BTC: A2DP Enable with AVRC
I (2576) BT_AUD_HOST: GAP event: 10
I (2578) BT_AUD_HOST: GAP event: 10
I (2579) BT_AUD_HOST: GAP event: 10
I (2581) BT_AUD_A2D_SINK: bt_a2d_event_cb unhandled event: 5
I (2586) BT_AUD_A2D_SINK: A2DP sink: initialized
I (2594) BT_AUD_HOST: GAP event: 10
I (2596) BT_AUD_HFP_HF: HF client init success
I (2599) BT_AUD_HOST: Setting bluedroid scan mode: connectable true, discoverable true
I (2606) BT_AUD_AVRC_CT: CT: Register notifications mask 0xff

Type 'help' to get the list of commands.
Use UP/DOWN arrows to navigate through command history.
Press TAB when typing command name to auto-complete.
I (2677) main_task: Returned from app_main()
BTAudio >
```

Connection and media control output may vary. To reduce log noise, use `esp_log_level_set()` in code.

### References

- [ESP Board Manager](https://github.com/espressif/esp-board-manager)
- [esp-bmgr-assist](https://github.com/espressif/esp-board-manager/blob/main/esp_board_manager/docs/esp_bmgr_assist.md)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/index.html)

## Troubleshooting

### microSD or Audio File Not Found (A2DP Source)

If the log shows file open or path errors, ensure the microSD card is mounted and the root directory contains `media0.mp3`, `media1.mp3`, and `media2.mp3` (or the filenames configured in code).

### Bluetooth Won't Connect or No Sound

- Confirm the Bluetooth role in menuconfig matches the other device (Sink with Source, Source with Sink)
- Confirm the devices are paired/connected and there are no connection or A2DP/AVRCP errors in the log
- For HFP, confirm the phone has granted phone and audio access
- For LE Audio, confirm the target supports BLE ISO/Bluetooth Audio, NimBLE and the required ESP-IDF LE Audio profile options are enabled, and the peer role matches the selected TMAP preset

### Build or Board-Related Errors

- Confirm you have installed `esp-bmgr-assist` (`pip install esp-bmgr-assist`), run `idf.py set-target esp32`, and run `idf.py bmgr -b <board>`
- For a custom board, see [How to customize board](https://github.com/espressif/esp-board-manager/blob/main/esp_board_manager/docs/how_to_customize_board.md)

### Cannot Retrieve Phonebook or Call History

- For Classic Bluetooth pairing, some phones require allowing the device to access the phonebook; check in Bluetooth settings whether this is authorized.

## Technical Support

For technical support, use the links below:

- Technical support: [esp32.com](https://esp32.com/viewforum.php?f=20) forum
- Issue reports and feature requests: [GitHub issue](https://github.com/espressif/esp-gmf/issues)

We will reply as soon as possible.
