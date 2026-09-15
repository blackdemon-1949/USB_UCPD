#!/usr/bin/env python3
"""Run one console command against the built image and wait for its answer.

The console is queued and flushed by the application's super loop, so a fixed
`--settle` budget is either wasteful or too short, and when several commands
run in one session the attribution of answers to commands gets fragile.  This
driver boots, injects exactly one command, then runs until the answer appears
(the handler's ``  <verb>`` reply line) or the instruction budget is spent, and
prints the whole console with the answer marked.

Usage:
    one_cmd.py <elf> "<command>" [--run-loop R] [--nor-image IMG] [--nor-out IMG]
               [--boot-budget N] [--runs N]
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("command")
    ap.add_argument("--boot-budget", type=int, default=40_000_000)
    ap.add_argument("--run-extra", type=int, default=12_000_000,
                    help="instructions to keep running after the command is injected")
    ap.add_argument("--nor-image", help="pre-load the modelled store window")
    ap.add_argument("--nor-out", help="save the modelled store window at the end")
    ap.add_argument("--runs", type=int, default=1,
                    help="inject the command this many times in one session")
    args = ap.parse_args()

    vb = load("vboard")
    nmod = load("vboard_nor")
    cli = load("vboard_cli")
    nmod.prepare(vb.Board)
    board = cli.Board(vb, args.elf)
    nor = nmod.install(board, load=args.nor_image, verbose=False)

    print(f"booting {args.elf} (budget {args.boot_budget:,} instructions) ...", flush=True)
    board.run(args.boot_budget, stop_when=lambda: "[boot] ready" in board.console())
    text = board.console()
    print(text[text.index("[USART2"):] if "[USART2" in text else text, flush=True)
    if "[boot] ready" not in text:
        print("!! never reached the ready banner", flush=True)

    reply_word = args.command.split()[0]
    for run in range(args.runs):
        before = len(board.console())
        if not board.inject(args.command + "\r\n"):
            print("!! the input ring would not take the bytes", flush=True)
            break
        answered = False
        for _ in range(60):
            board.run(args.run_extra // 60)
            answer = board.console()[before:]
            if reply_word in answer and answer.rstrip().endswith(">"):
                answered = True
                break
        text = board.console()[before:]
        lines = [ln for ln in text.replace("\r", "").splitlines()
                 if ln.strip() and not ln.startswith("[ina226] no ina226")]
        print(f"\n$ {args.command}   (run {run + 1}, {'answered' if answered else 'NO ANSWER'})")
        print("\n".join("  " + ln for ln in lines) or "  (no output)", flush=True)

    if args.nor_out:
        nor.save(args.nor_out)
        print(f"\nstore window saved to {args.nor_out}", flush=True)
    print(nor.summary(), flush=True)
    print("fatal:", board.fatal or "none", flush=True)
    print(f"instructions: {board.count:,}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
