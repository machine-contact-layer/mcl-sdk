#!/usr/bin/env python3
"""Copy and relocate the Markdown reference closure into the generated SDK.

Normative/profile documents and text references are shipped locally. Binary
research artifacts stay explicit external evidence links; they are not inputs
to building or using the package. Missing source references fail the build.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import unquote

LINK = re.compile(r"(?P<prefix>\]\()(?P<target>[^)\n]+)(?P<suffix>\))")
REFERENCE_LINK = re.compile(r'(?m)^(\s*\[[^\]\n]+\]:\s*)(\S+)')
TEXT_REFERENCE = re.compile(r"(?<!\[)`((?:\.\./)*mcl-(?:core|wire|link|sdk|ap|ble|ip|uwb)/[^`\s]*)`(?!\])")
SEEDS = {
    "SECURITY.md": "mcl-core/SECURITY.md",
    "REPORTING.md": "mcl-core/REPORTING.md",
    "SPECIFICATION_INDEX.md": "mcl-core/SPECIFICATION_INDEX.md",
    "V1_SCOPE.md": "mcl-core/governance/V1_SCOPE.md",
    "conformance-profiles-v1.md": "mcl-core/spec/conformance-profiles-v1.md",
    "deployment-profile-v1.md": "mcl-core/spec/deployment-profile-v1.md",
    "SPEC_GAPS.md": "mcl-core/conformance/independent/SPEC_GAPS.md",
    "TWO_BUILDER_AUDIT.md": "mcl-core/research/TWO_BUILDER_AUDIT.md",
    "ap-bootstrap-1.md": "mcl-ap/spec/ap-bootstrap-1.md",
    "ble-activate-1.md": "mcl-ble/spec/ble-activate-1.md",
    "ble-gatt-profile-v1.md": "mcl-ble/spec/ble-gatt-profile-v1.md",
}


def source_revision(repo):
    """Resolve an exact revision in both checkouts and git-archive exports."""
    if (repo / '.git').exists():
        value = subprocess.check_output(
            ['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    else:
        metadata = repo / '.git-archive-revision'
        value = metadata.read_text(encoding='utf-8').strip() if metadata.is_file() else ''
    if not re.fullmatch(r'[0-9a-f]{40}', value):
        raise ValueError(f'missing exact source revision for {repo.name}')
    return value


def targets(text):
    # Literal Markdown syntax in fenced examples is not a link.
    text = re.sub(r"(?ms)^```[^\n]*\n.*?^```[^\n]*$", "", text)
    return ([m.group('target') for m in LINK.finditer(text)] +
            [m[2] for m in REFERENCE_LINK.finditer(text)])


def check(package):
    if not package.is_dir():
        raise ValueError(f"missing package: {package}")
    count = 0
    for doc in package.rglob('*.md'):
        text = doc.read_text(encoding='utf-8')
        for target in targets(text):
            if target.startswith(('https://', 'http://', 'mailto:', '#')):
                continue
            local = (doc.parent / unquote(target.split('#')[0])).resolve()
            if not local.is_relative_to(package) or not local.exists():
                raise ValueError(f"unresolved packaged link: {doc.relative_to(package)} -> {target}")
            count += 1
        if TEXT_REFERENCE.search(text):
            raise ValueError(f"source-tree reference in {doc.relative_to(package)}")
    print(f"Packaged Markdown: {count} local links verified across all documents")


def assemble(root, package):
    mapping = {(root / source).resolve(): package / 'docs' / name
               for name, source in SEEDS.items()}
    for name in ('QUICKSTART.md', 'BUILDER_GUIDE.md', 'PORTING.md', 'RESOURCE_ENVELOPE.md'):
        mapping[(root / 'mcl-sdk' / name).resolve()] = package / name
    reference_root = package / 'docs' / 'reference'
    for copied in reference_root.rglob('*.md'):
        mapping[(root / copied.relative_to(reference_root)).resolve()] = copied
    pending = list(mapping)
    revisions = {}

    def locate(target, source):
        path = unquote(target.split('#')[0])
        candidates = [source.parent / path]
        if path.startswith('mcl-'):
            candidates.insert(0, root / path)
        repo = source.relative_to(root).parts[0]
        candidates.append(root / repo / path)
        for candidate in candidates:
            candidate = candidate.resolve()
            if candidate.is_relative_to(root) and candidate.exists():
                return candidate
        return None

    def relocate(target, source, destination):
        if target.startswith(('https://', 'http://', 'mailto:', '#')):
            return target
        line_number = re.search(r':(\d+)(?:-\d+)?$', target)
        if line_number:
            target = target[:line_number.start()] + '#L' + line_number[1]
        anchor = '#' + target.split('#', 1)[1] if '#' in target else ''
        local = (destination.parent / target.split('#')[0]).resolve()
        if local.is_relative_to(package) and local.exists():
            return target
        original = locate(target, source)
        if original is None:
            raise ValueError(f"missing source link: {source.relative_to(root)} -> {target}")
        if original.is_dir() and (original / 'README.md').is_file():
            original = original / 'README.md'
        if original.is_file() and original.suffix.lower() in (
                '.md', '.txt', '.json', '.csv', '.yaml', '.yml', '.c', '.h'):
            if original not in mapping:
                mapped = package / 'docs' / 'reference' / original.relative_to(root)
                mapping[original] = mapped
                mapped.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(original, mapped)
                if original.suffix.lower() == '.md':
                    pending.append(original)
            return Path(os.path.relpath(mapping[original], destination.parent)).as_posix() + anchor
        repo, *relative = original.relative_to(root).parts
        if repo not in revisions:
            revisions[repo] = source_revision(root / repo)
        kind = 'tree' if original.is_dir() else 'blob'
        return f"https://github.com/machine-contact-layer/{repo}/{kind}/{revisions[repo]}/{'/'.join(relative)}{anchor}"

    for source in pending:
        destination = mapping[source]
        text = destination.read_text(encoding='utf-8')
        text = LINK.sub(lambda m: m['prefix'] + relocate(m['target'], source, destination) + m['suffix'], text)
        text = REFERENCE_LINK.sub(lambda m: m[1] + relocate(m[2], source, destination), text)
        # Repository-qualified inline references must not name absent siblings.
        def inline_reference(match):
            reference = match[1]
            brace = re.search(r'\{([^}]+)\}', reference)
            references = ([reference[:brace.start()] + choice + reference[brace.end():]
                           for choice in brace[1].split(',')] if brace else [reference])
            if '*' in reference:
                references = [item.relative_to(root).as_posix() for item in sorted(root.glob(reference))]
                if not references:
                    raise ValueError(f"empty source reference: {reference}")
            return ', '.join('[' + Path(item.rstrip('/')).name + '](' +
                             relocate(item, source, destination) + ')' for item in references)
        text = TEXT_REFERENCE.sub(inline_reference, text)
        destination.write_text(text, encoding='utf-8', newline='\n')
    check(package)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--root', type=Path)
    args = parser.parse_args()
    if args.root:
        assemble(args.root.resolve(), args.package.resolve())
    else:
        check(args.package.resolve())
