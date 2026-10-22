"""Split oversized lifted C at function boundaries; keep generated game code local."""
import argparse
from pathlib import Path
import re


def split(source: Path, output: Path, limit: int):
    text = source.read_text(encoding="utf-8")
    starts = list(re.finditer(r"^void sub_[0-9A-Fa-f]+\(void\) \{", text, re.MULTILINE))
    if not starts:
        raise ValueError(f"No lifted functions in {source}")
    header = text[:starts[0].start()]
    functions = [text[start.start():starts[i+1].start() if i+1 < len(starts) else len(text)]
                 for i, start in enumerate(starts)]
    groups, current, size = [], [], 0
    for function in functions:
        if current and size + len(function) > limit:
            groups.append("".join(current))
            current, size = [], 0
        current.append(function)
        size += len(function)
    if current:
        groups.append("".join(current))
    if header + "".join(groups) != text:
        raise ValueError("Splitting changed the generated source")
    output.mkdir(parents=True, exist_ok=True)
    paths = []
    for i, group in enumerate(groups):
        path = output / f"{source.stem}_part_{i:03d}.c"
        content = header + group
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            path.write_text(content, encoding="utf-8", newline="\n")
        paths.append(path.resolve())
    return paths


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--max-bytes", type=int, default=6 * 1024 * 1024)
    args = parser.parse_args()
    if args.max_bytes <= 0:
        parser.error("--max-bytes must be positive")
    print("\n".join(path.as_posix() for path in split(args.source, args.output, args.max_bytes)))
