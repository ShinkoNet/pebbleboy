#!/usr/bin/env python3
"""Summarize Pebbleboy ROM bank activity from emulator logs."""

import argparse
import collections
import re
import sys


LOAD_RE = re.compile(r"cart: (resource|memory|phone) bank (\d+)(?: (?:page|fill) \d+)? loaded")
PHONE_REQUEST_RE = re.compile(r"cart: phone request bank (\d+) fill (\d+) size=(\d+)( prefetch)?")
PHONE_LATENCY_RE = re.compile(r"phone bank (\d+) ready fill=(\d+) size=(\d+) latency_ms=(\d+)")


def bank_set(values):
    if not values:
        return "none"
    return ",".join(str(v) for v in sorted(set(values)))


def top_counts(counter):
    if not counter:
        return "none"
    return ",".join(f"{bank}:{count}" for bank, count in counter.most_common(8))


def latency_summary(values):
    if not values:
        return "none"
    return f"{min(values)}/{sum(values) // len(values)}/{max(values)}"


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
    parser.add_argument("--expect-phone-request-count", type=int)
    parser.add_argument("--expect-phone-request-size", type=int)
    parser.add_argument("--expect-phone-request-sizes")
    parser.add_argument("--expect-phone-latencies", action="store_true")
    parser.add_argument("--allow-pending-phone-request", action="store_true")
    parser.add_argument("--max-phone-latency-ms", type=int)
    parser.add_argument("--max-demand-phone-latency-ms", type=int)
    args = parser.parse_args()

    loads = collections.defaultdict(list)
    phone_requests = []
    phone_request_sizes = []
    phone_latencies = []
    phone_demand_latencies = []
    phone_prefetch_latencies = []
    pending_requests = collections.defaultdict(collections.deque)
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
                bank = int(request_match.group(1))
                offset = int(request_match.group(2))
                size = int(request_match.group(3))
                kind = "prefetch" if request_match.group(4) else "demand"
                phone_requests.append(bank)
                phone_request_sizes.append(size)
                pending_requests[(bank, offset, size)].append(kind)

            latency_match = PHONE_LATENCY_RE.search(line)
            if latency_match:
                bank = int(latency_match.group(1))
                offset = int(latency_match.group(2))
                size = int(latency_match.group(3))
                latency = int(latency_match.group(4))
                phone_latencies.append(latency)
                queue = pending_requests.get((bank, offset, size))
                kind = queue.popleft() if queue else "unknown"
                if kind == "prefetch":
                    phone_prefetch_latencies.append(latency)
                elif kind == "demand":
                    phone_demand_latencies.append(latency)

    request_counts = collections.Counter(phone_requests)
    print(
        "bank log: "
        f"loads resource={bank_set(loads['resource'])} "
        f"memory={bank_set(loads['memory'])} "
        f"phone={bank_set(loads['phone'])} "
        f"phone_requests={len(phone_requests)} "
        f"request_banks={bank_set(phone_requests)} "
        f"request_sizes={bank_set(phone_request_sizes)} "
        f"latency_ms_min_avg_max={latency_summary(phone_latencies)} "
        f"demand_latency_ms_min_avg_max={latency_summary(phone_demand_latencies)} "
        f"prefetch_latency_ms_min_avg_max={latency_summary(phone_prefetch_latencies)} "
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
    if (args.expect_phone_request_count is not None and
            len(phone_requests) != args.expect_phone_request_count):
        return fail(
            f"expected {args.expect_phone_request_count} phone requests; saw {len(phone_requests)}"
        )
    if args.expect_phone_request_size is not None:
        bad_sizes = [size for size in phone_request_sizes if size != args.expect_phone_request_size]
        if bad_sizes:
            return fail(
                f"expected phone request size {args.expect_phone_request_size}; "
                f"saw {bank_set(phone_request_sizes)}"
            )
    if args.expect_phone_request_sizes:
        expected_sizes = {
            int(part) for part in args.expect_phone_request_sizes.split(",") if part
        }
        bad_sizes = [size for size in phone_request_sizes if size not in expected_sizes]
        if bad_sizes:
            return fail(
                f"expected phone request sizes within {bank_set(expected_sizes)}; "
                f"saw {bank_set(phone_request_sizes)}"
            )
    if args.expect_phone_latencies and len(phone_latencies) != len(phone_requests):
        pending = len(phone_requests) - len(phone_latencies)
        if not (args.allow_pending_phone_request and pending == 1):
            return fail(
                f"expected one phone latency per request; saw "
                f"{len(phone_latencies)} latencies for {len(phone_requests)} requests"
            )
    if args.max_phone_latency_ms is not None:
        bad_latencies = [value for value in phone_latencies if value > args.max_phone_latency_ms]
        if bad_latencies:
            return fail(
                f"expected phone latencies <= {args.max_phone_latency_ms}ms; "
                f"saw {max(bad_latencies)}ms"
            )
    if args.max_demand_phone_latency_ms is not None:
        bad_latencies = [
            value for value in phone_demand_latencies
            if value > args.max_demand_phone_latency_ms
        ]
        if bad_latencies:
            return fail(
                f"expected demand phone latencies <= {args.max_demand_phone_latency_ms}ms; "
                f"saw {max(bad_latencies)}ms"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
