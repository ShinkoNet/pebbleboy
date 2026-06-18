#!/usr/bin/env python3
"""Summarize Pebbleboy ROM bank activity from emulator logs."""

import argparse
import collections
import re
import sys


LOAD_RE = re.compile(r"cart: (resource|memory|phone) bank (\d+)(?: (?:page|fill) \d+)? loaded")
PHONE_REQUEST_RE = re.compile(r"cart: phone request bank (\d+)")


def bank_set(values):
    if not values:
        return "none"
    return ",".join(str(v) for v in sorted(set(values)))


def top_counts(counter):
    if not counter:
        return "none"
    return ",".join(f"{bank}:{count}" for bank, count in counter.most_common(8))


def fail(message):
    print(message, file=sys.stderr)
    return 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--expect-local-tetris", action="store_true")
    parser.add_argument("--expect-no-resource", action="store_true")
    parser.add_argument("--expect-phone-title")
    parser.add_argument("--expect-phone-banks")
    args = parser.parse_args()

    loads = collections.defaultdict(list)
    phone_requests = []
    started_tetris = False
    phone_titles = []
    phone_offer = False
    phone_start = False

    with open(args.log, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "started TETRIS local" in line:
                started_tetris = True
            if "phone info title=" in line or "switching from " in line:
                phone_offer = True
            if "started " in line and " phone," in line:
                phone_start = True
                title_match = re.search(r"started (.+) phone,", line)
                if title_match:
                    phone_titles.append(title_match.group(1))

            load_match = LOAD_RE.search(line)
            if load_match:
                loads[load_match.group(1)].append(int(load_match.group(2)))
                continue

            request_match = PHONE_REQUEST_RE.search(line)
            if request_match:
                phone_requests.append(int(request_match.group(1)))

    request_counts = collections.Counter(phone_requests)
    print(
        "bank log: "
        f"loads resource={bank_set(loads['resource'])} "
        f"memory={bank_set(loads['memory'])} "
        f"phone={bank_set(loads['phone'])} "
        f"phone_requests={len(phone_requests)} "
        f"request_banks={bank_set(phone_requests)} "
        f"top_requests={top_counts(request_counts)}"
    )

    if args.expect_local_tetris:
        resource_banks = set(loads["resource"])
        if not started_tetris:
            return fail("local Tetris did not start")
        if resource_banks != {0, 1}:
            return fail(f"expected local Tetris resource banks 0,1; saw {bank_set(resource_banks)}")
        if phone_offer or phone_start or phone_requests:
            return fail("local Tetris smoke unexpectedly accepted or requested a phone ROM")

    if args.expect_no_resource and loads["resource"]:
        return fail(f"expected no resource ROM loads; saw {bank_set(loads['resource'])}")

    if args.expect_phone_title and args.expect_phone_title not in phone_titles:
        return fail(f"expected phone title {args.expect_phone_title}; saw {phone_titles or ['none']}")

    if args.expect_phone_banks:
        expected = {int(part) for part in args.expect_phone_banks.split(",") if part}
        actual = set(loads["phone"])
        if actual != expected:
            return fail(f"expected phone banks {bank_set(expected)}; saw {bank_set(actual)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
