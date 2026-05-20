#!/usr/bin/env python3

import argparse
import json
import sys
from pathlib import Path


DEFAULT_SOURCE_COLUMN = "creation_sourcecode"


def normalize_address(address):
    return address.lower().removeprefix("0x")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Export one apehex parquet source record to a markdown file."
    )
    parser.add_argument("parquet", type=Path, help="apehex parquet file")
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument(
        "--contract-address",
        help="contract_address value to export, with or without 0x",
    )
    selector.add_argument(
        "--row-index",
        type=int,
        help="zero-based row index in the parquet file",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        required=True,
        help="output markdown path",
    )
    parser.add_argument(
        "--source-column",
        default=DEFAULT_SOURCE_COLUMN,
        help=f"source column name, default: {DEFAULT_SOURCE_COLUMN}",
    )
    return parser.parse_args()


def import_pyarrow():
    try:
        import pyarrow.parquet as pq
    except ImportError as exc:
        raise SystemExit("pyarrow is required to read parquet files") from exc
    return pq


def read_columns(parquet, source_column):
    pq = import_pyarrow()
    columns = [
        "block_number",
        "contract_address",
        "transaction_hash",
        source_column,
    ]
    try:
        return pq.read_table(parquet, columns=columns).to_pydict()
    except Exception as exc:
        raise SystemExit(f"failed to read parquet columns: {exc}") from exc


def find_row(rows, args):
    if args.row_index is not None:
        if args.row_index < 0 or args.row_index >= len(rows["contract_address"]):
            raise SystemExit(f"row index out of range: {args.row_index}")
        return args.row_index

    wanted = normalize_address(args.contract_address)
    for index, address in enumerate(rows["contract_address"]):
        if normalize_address(address) == wanted:
            return index
    raise SystemExit(f"contract address not found: {args.contract_address}")


def parse_source_json(raw_source):
    if not raw_source:
        raise SystemExit("source column is empty for selected row")

    # Some apehex records wrap the Solidity standard JSON input in one extra
    # brace pair, producing strings like "{{ ... }}". Try the raw text first,
    # then the unwrapped form.
    candidates = [raw_source]
    if raw_source.startswith("{{") and raw_source.endswith("}}"):
        candidates.append(raw_source[1:-1])

    last_error = None
    for candidate in candidates:
        try:
            return json.loads(candidate)
        except json.JSONDecodeError as exc:
            last_error = exc

    raise SystemExit(f"failed to parse source JSON: {last_error}")


def code_fence_for(content):
    fence = "```"
    while fence in content:
        fence += "`"
    return fence


def write_markdown(output, parquet, rows, row_index, source_column, source_json):
    sources = source_json.get("sources", {})
    if not sources:
        raise SystemExit("parsed source JSON has no sources")

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as file:
        file.write("# Apehex Solidity Sources\n\n")
        file.write(f"- parquet: `{parquet}`\n")
        file.write(f"- row_index: `{row_index}`\n")
        file.write(f"- source_column: `{source_column}`\n")
        file.write(f"- block_number: `{rows['block_number'][row_index]}`\n")
        file.write(
            f"- contract_address: `{rows['contract_address'][row_index]}`\n"
        )
        file.write(
            f"- transaction_hash: `{rows['transaction_hash'][row_index]}`\n"
        )
        file.write(f"- language: `{source_json.get('language', '')}`\n")
        file.write(f"- files: `{len(sources)}`\n\n")

        for name, source in sources.items():
            content = source.get("content", "")
            fence = code_fence_for(content)
            file.write(f"## {name}\n\n")
            file.write(f"{fence}solidity\n")
            file.write(content)
            if content and not content.endswith("\n"):
                file.write("\n")
            file.write(f"{fence}\n\n")


def main():
    args = parse_args()
    rows = read_columns(args.parquet, args.source_column)
    row_index = find_row(rows, args)
    source_json = parse_source_json(rows[args.source_column][row_index])
    write_markdown(
        args.output,
        args.parquet,
        rows,
        row_index,
        args.source_column,
        source_json,
    )
    print(args.output)


if __name__ == "__main__":
    sys.exit(main())
