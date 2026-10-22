# The Circle Pad Pro device (N3-08)

Azahar connects device 1 of `ir:USER` to an emulated Circle Pad Pro (`ExtraHID`), whatever the input
settings. `mh4n::ir_device::CirclePadPro` (`native/src/ir_device/`) is that device's IR protocol as a
module of its own: it knows nothing of the service, the kernel or the shared memory. N3-12 plugs it into
`ir:USER`; N3-14 and the input source feed it an `InputState`.

| File | What |
|---|---|
| `native/src/ir_device/circle_pad_pro.{h,cpp}` | the device (ported from `extra_hid.cpp`, with provenance) |
| `native/tests/ir_device_test.cpp` | its unit tests (`ctest -R '^ir_device_test$'`) |
| `port/tests/ir.h`, `port/tests/ir_cpp.c` | our own 3DS program: the application side of `ir:USER` |
| `port/tests/expected/ir/ir_cpp.txt` | the reference runner's output (Azahar's code) |
| `tests/test_native_ir_cpp.py` | builds and runs the program |

## The protocol

The game talks to the device with `SendIrNop` (the payload reaches `OnReceive`); the device answers, and
sends its status periodically, by calling the `send` callback with a bare payload. The service wraps each
payload into a packet of the receive buffer (`0xA5`, the network id, a one or two byte size, the payload,
a CRC-8) and signals the receive event: that wrapping (Azahar's `IR_USER::PutToReceive`) is N3-12's.

| Request (game to device) | Layout | Effect |
|---|---|---|
| `1` ConfigureHIDPolling | `[1, period in ms, unknown]`, exactly 3 bytes | cancels the pending event, remembers the period, and sends the first status packet one period later, then one every period |
| `2` ReadCalibrationData | `[2, expected response time, offset u16, size u16]`, exactly 6 bytes | offset and size are aligned down to 16; if `offset + size` is past the 0x40 bytes nothing is sent; otherwise the answer below |
| anything else | | logged once per request id, no answer |

| Response (device to game) | Layout |
|---|---|
| `0x11` calibration | `[0x11, offset u16, size u16, size bytes of the calibration data at the aligned offset]`; offset and size are echoed as the game wrote them (not aligned) |
| `0x10` status | 6 bytes: `0x10`; the stick x (12 bits) and y (12 bits) as two little-endian fields in bytes 1 to 3; byte 4: bits 0 to 4 the battery level (`0x1F`), bit 5 ZL, bit 6 ZR, bit 7 R, each set when the button is **not** pressed (R never is); byte 5 unknown (0) |

With no input the stick is centred, not zero: x = y = 0x800, so the status packet is
`10 00 08 80 ff 00`. The calibration data are the 64 bytes Azahar says it "retrieved from a New 3DS".

### From `InputState` to the stick

`InputState::cstick_x/y` are in the units of `ir_rst.cpp` (radius `0x9C`: `static_cast<s16>(f * 0x9C)`).
The device uses the expression of `extra_hid.cpp`'s artic branch, which takes the same raw units:
`static_cast<u32>((raw / 156.0f) * 2047 + 2048)`, in float, so raw `+156` is `0xFFF`, `-156` is `0x001`,
0 is `0x800`. Azahar's own branch for the local controller does the same with the analog device's
`x` in [-1, 1] (`0x800 + 0x7FF * x`); the two agree for the values `ir_rst.cpp` produces. A raw value
beyond +-156 (nothing produces one) is held to +-156, because the 12-bit field would wrap. `zl` and `zr`
come from the same `InputState`; its other buttons do not reach the device (the Circle Pad Pro's R is
never reported held, as in Azahar).

### Time

The device holds no clock. The `Timer` it is given (N3-12 binds it to the service's timer queue) runs a
callback once, after a number of game-clock ticks: `kTicksPerSecond * period / 1000` (Azahar's
`msToCycles(int)`, rounded down: 4,289,789 ticks for 16 ms). The next packet is scheduled from inside the
callback with the whole period; Azahar subtracts the lateness of the event (`cycles_late`), so a timer
queue whose clock is past the event's due time should take the lateness off the delay it is asked for.
`Cancel` must drop a pending callback; a callback that runs after a disconnect does nothing anyway.

### Where this differs from Azahar

- A polling period of **0** stops the packets (Azahar schedules an event after 0 cycles that reschedules
  itself after 0 cycles, so time cannot advance). The game can start them again with another
  configuration.
- An **empty** request is logged and ignored (Azahar reads `data[0]`).
- Unknown requests and wrong request sizes are logged once per request id, not each time.
- Connecting again after a disconnect does not restart the packets until the game configures the polling
  again: that is Azahar's behaviour too (`OnConnect` does nothing).
- Azahar reloads its input devices when the settings change; here the `InputState` is sampled for every
  packet.

## `ir_cpp`: the oracle program

`port/tests/ir_cpp.c` (`port/tests/ir.h` has the `ir:USER` calls and the receive buffer's layout) is
built and run as `tests/test_conformance.py` does (the helpers are imported, the program is not in its
`PROGRAMS`). It:

1. commits heap memory, makes a shared memory block over it, `InitializeIrNopShared` (receive buffer
   0x400 bytes, 8 packets), gets the three events;
2. `RequireConnection(7)` (an unknown device: Azahar answers success and marks the header "trying to
   connect"), then `RequireConnection(1)` and waits for the connection event; prints the header;
3. `SendIrNop`: the polling configuration (20 ms), the calibration read of the whole 0x40 bytes, a read at
   offset 0x13 for 0x1F bytes (aligned down and echoed as written), and three requests Azahar ignores (a
   read past the end, a configuration of 2 bytes, an unknown request); their results are printed;
4. receives packets one by one (`ReleaseReceivedData` after each; the packet's `0xA5`, size and CRC-8 are
   checked): prints the two calibration answers and the first 50 status packets as hex, and checks that
   the 49 periods between the first and the last last 980 ms within 5 % (a check that holds on either
   clock; the packet timing itself is not printed);
5. `Disconnect`, drops what is in the buffer, and checks that the receive event stays quiet for 100 ms;
   `SendIrNop` after the disconnect (the error result Azahar gives), `FinalizeIrNop`.

Output: `ir_cpp: start`, one line per item (`ir_cpp: <name>: <8 hex digits>` for a value,
`ir_cpp: calibration <hex>`, `ir_cpp: pkt NN <hex>`), `ir_cpp: done`. A line with `FAIL` is a failure.

## Running and regenerating

```
# reference only (the expected file is Azahar's output)
MH4U_RUNNER=<reference runner> MH4N_IR_REQUIRE=reference python3 -m pytest -rs tests/test_native_ir_cpp.py
# both runners; the native one is required from N3-12 on
MH4U_RUNNER=... MH4N_RUNNER=<native runner> MH4N_IR_REQUIRE=both python3 -m pytest -rs tests/test_native_ir_cpp.py
# rewrite port/tests/expected/ir/ir_cpp.txt (two identical reference runs are needed)
MH4U_RUNNER=<reference runner> python3 tests/test_native_ir_cpp.py --regenerate
```

`MH4N_IR_REQUIRE` holds `reference`, `native` or `both`: a runner it names that is not set is a failure,
an unnamed one a skip. With no runner set the tests that need one are skipped and the rest (the expected
file is well formed, says what Azahar's code makes, and the program builds) run.
