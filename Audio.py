"""
captcha_audio.py
================
Audio companion for the Elmo Game.

The phone web page now controls strikes (HAND / FOOT buttons), not the keyboard.
This script listens to D board's Serial output and speaks the matching voice line.

Strings emitted by D board that we react to:
  ONBOARDING


  0p  
  CALIBRATING              # panels lighting up; speaks "Don't hit. Calibrating. 3. 2. 1. Go!"
  COUNTDOWN_3 / COUNTDOWN_2 / COUNTDOWN_1   # used by D simulation only
  STRIKE_HAND_1 / STRIKE_HAND_2 / STRIKE_FINAL_HAND
  STRIKE_FOOT_1 / STRIKE_FOOT_2 / STRIKE_FINAL_FOOT
  STRIKE_WRONG_1 / STRIKE_WRONG_2 / STRIKE_FINAL_WRONG
  SUCCESS / FAIL / PERFECT / CHEAT
  RESET_ALL                # tells us to clear result_done so a new game can talk

How to run:
  1. Edit `port` below to match your D board's serial port.
  2. python3 captcha_audio.py
  3. Use the phone web page (http://192.168.4.1) to play.

Requires: pyserial  (pip install pyserial)
macOS `say` is used for TTS. On Linux/Windows see the say() function for swaps.
"""

import serial
import subprocess
import time

port = "/dev/cu.usbmodem5AF61129291"
baud = 115200
VOICE = "Daniel"

result_done = False
onboarding_done = False   # True after ONBOARDING has been spoken once; cleared by RESET_ALL


def say(text):
    subprocess.run(["say", "-v", VOICE, text])


def get_volume():
    try:
        out = subprocess.check_output(
            ["osascript", "-e", "output volume of (get volume settings)"],
            text=True
        ).strip()
        return int(out)
    except Exception:
        return 50


def set_volume(value):
    value = max(0, min(100, int(value)))
    subprocess.run(["osascript", "-e", f"set volume output volume {value}"])


def volume_up(step=10):
    set_volume(get_volume() + step)


def volume_down(step=10):
    set_volume(get_volume() - step)


def open_serial():
    while True:
        try:
            s = serial.Serial(port, baud, timeout=1)
            print(f"Connected to {port}")
            return s
        except serial.SerialException as e:
            print(f"Serial open failed ({e}); retrying in 2s...")
            time.sleep(2)


def handle_line(line: str):
    global result_done, onboarding_done

    if not line:
        return

    print(f"Received: {line}")

    if line == "NO_FEET":
        say("You're cheating. No feet on the floor.")
        return

    if line == "VOL_UP":
        volume_up()
        return

    if line == "VOL_DOWN":
        volume_down()
        return

    if line == "RESET_ALL":
        result_done = False
        onboarding_done = False   # allow ONBOARDING to play again for the next game
        return

    if line == "ONBOARDING":
        # ONBOARDING is allowed once per game.
        # The lock is cleared only when RESET_ALL arrives, so any
        # duplicate ONBOARDING strings emitted by A/B/C boot sequence
        # are silently ignored.
        if onboarding_done:
            print("Ignored duplicate ONBOARDING (already spoken this game)")
            return

        onboarding_done = True
        result_done = False

        say("Elmo's Circus is auditioning.")
        say("But first, you must prove you're worthy.")
        return

    if line == "CALIBRATING":
        # Plays right after onboarding finishes (Python processes this from the queue).
        # say() is blocking, so each call acts as natural spacing.
        # Total speech ~4-5 s, landing just as the last panel finishes (~12.9 s cal time).
        say("Remember, the system is watching.")
        say("3")
        say("2")
        say("1")
        say("Hit the panels now.")
        return

    if line == "STRIKE_HAND_1":
        say("Strike 1. That was a hand panel.")
        return

    if line == "STRIKE_HAND_2":
        say("Strike 2. That was a hand panel.")
        return

    if line == "STRIKE_FOOT_1":
        say("Strike 1. That was a foot panel.")
        return

    if line == "STRIKE_FOOT_2":
        say("Strike 2. That was a foot panel.")
        return

    if line == "STRIKE_WRONG_1":
        say("Strike 1. Cheating detected. Hit the correct panel.");  return
    if line == "STRIKE_WRONG_2":
        say("Strike 2. Cheating detected. Hit the correct panel.");  return

    if line == "STRIKE_FINAL_WRONG" and not result_done:
        result_done = True
        say("Strike 3. Cheating detected. Hit the correct panel.")
        say("You broke the system. No puppet would do that.")
        say("Unfortunately, you're too human for us.")
        return

    if line == "STRIKE_FINAL_HAND" and not result_done:
        result_done = True
        say("Strike 3. That was a hand panel.")
        say("You broke the system. No puppet would do that.")
        say("Unfortunately, you're too human for us.")
        return

    if line == "STRIKE_FINAL_FOOT" and not result_done:
        result_done = True
        say("Strike 3. That was a foot panel.")
        say("You broke the system. No puppet would do that.")
        say("Unfortunately, you're too human for us.")
        return

    if line == "SUCCESS" and not result_done:
        result_done = True
        say("You proved your humanity. By doing exactly what you were told.")
        say("You're not what we're looking for.")
        return

    if line == "FAIL" and not result_done:
        result_done = True
        say("You couldn't even prove you were human. A puppet through and through.")
        say("Welcome to Elmo's Circus.")
        return

    # PERFECT: natural 20/20 score — obedient puppet ending
    if line == "PERFECT" and not result_done:
        result_done = True
        say("A perfect score. No human is that obedient.")
        say("We see you. Puppet.")
        return

    if line == "CHEAT" and not result_done:
        result_done = True
        say("You broke the system. No puppet would do that.")
        say("Unfortunately, you're too human for us.")
        return


def main():
    ser = open_serial()
    print("Listening. Use the phone (http://192.168.4.1) to play.\n")

    try:
        while True:
            try:
                raw = ser.readline()
            except serial.SerialException as e:
                print(f"Serial error: {e}. Re-opening...")
                ser.close()
                ser = open_serial()
                continue

            line = raw.decode("utf-8", errors="ignore").strip()
            handle_line(line)

    except KeyboardInterrupt:
        print("\nStopped.")
        ser.close()


if __name__ == "__main__":
    main()
