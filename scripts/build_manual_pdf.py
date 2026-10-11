# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
"""Render a docs/ manual (Markdown) to PDF: Markdown -> print HTML -> Edge headless.

Usage (IDF python works; it only needs the `markdown` package):
    python scripts/build_manual_pdf.py docs/MANUAL.md            # -> docs/MANUAL.pdf
    python scripts/build_manual_pdf.py docs/MANUAL.md out/MANUAL.pdf

    pip install markdown        # once, if `import markdown` fails

A manual may open with a raw-HTML `<div class="cover">` block (see
MANUAL.md); it becomes a full-bleed first page with no footer. Every other
page carries the copyright notice and a page number in the footer.

Links to other repository files become plain text: in a PDF they would point
at the build machine's own file system.

Edge prints a stale HTML file happily, so the HTML is deleted and rewritten
first, and the PDF's timestamp is checked afterwards.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import markdown
    markdown.markdown  # an empty namespace package named `markdown` can shadow the real one
except (ImportError, AttributeError):
    sys.exit("needs the `markdown` package: python -m pip install markdown")

NOTICE = ("Copyright \\00A9  2026 SigPro Labs, LLC. "
          "RadioSonic and SigPro Labs are trademarks of SigPro Labs, LLC.")

CSS = f"""
@page {{
  size: Letter; margin: 18mm 16mm 20mm 16mm;
  @bottom-left  {{ content: "{NOTICE}"; font: 7.5pt "Segoe UI", Arial, sans-serif; color: #666; }}
  @bottom-right {{ content: "Page " counter(page) " of " counter(pages);
                   font: 7.5pt "Segoe UI", Arial, sans-serif; color: #666; }}
}}
@page :first {{ margin: 0; @bottom-left {{ content: none; }} @bottom-right {{ content: none; }} }}
* {{ -webkit-print-color-adjust: exact; print-color-adjust: exact; }}
body {{ margin: 0; font: 10pt/1.45 "Segoe UI", Arial, sans-serif; color: #1a1a1a; }}

.cover {{
  width: 8.5in; height: 11in; box-sizing: border-box; padding: 0.85in 0.8in 0.55in;
  background: #c9dbe7;
  display: flex; flex-direction: column; align-items: center; text-align: center;
  break-after: page; overflow: hidden;
}}
.cover p {{ margin: 0; }}
.cover-name {{ font: 700 54pt "Segoe UI", Arial, sans-serif; color: #203864; margin-top: 0.3in !important; }}
.cover-title {{ font: 700 25pt "Segoe UI", Arial, sans-serif; margin-top: 0.4in !important; }}
.cover-edition {{ font: 600 15pt "Segoe UI", Arial, sans-serif; color: #203864; margin-top: 0.06in !important; }}
.cover-board {{ width: 5.5in; margin: 0.45in 0 auto; }}
.cover-by {{ font: 600 15pt "Segoe UI", Arial, sans-serif; }}
.cover-meta {{ font-size: 9pt; color: #2b2b2b; margin-top: 0.22in !important; }}

h1 {{ font-size: 20pt; border-bottom: 2px solid #333; padding-bottom: 4px; }}
h2 {{ font-size: 14pt; border-bottom: 1px solid #bbb; padding-bottom: 2px; margin-top: 1.4em; break-after: avoid; }}
h3 {{ font-size: 11.5pt; margin-top: 1.1em; break-after: avoid; }}
h4 {{ font-size: 10pt; margin: 1em 0 .3em; break-after: avoid; }}
/* A marker before a heading starts that heading on a new page. Breaking before the
   heading (rather than after the marker) never leaves a blank page. */
.page-break {{ display: none; }}
.page-break + h2, .page-break + h3 {{ break-before: page; }}
code {{ font: 8.8pt Consolas, "Courier New", monospace; background: #f2f2f2; padding: 0 2px; }}
pre {{ background: #f5f5f5; border: 1px solid #ddd; padding: 6px 8px; break-inside: avoid; white-space: pre-wrap; }}
pre code {{ background: none; padding: 0; }}
table {{ border-collapse: collapse; margin: .6em 0; width: 100%; font-size: 9pt; }}
th, td {{ border: 1px solid #ccc; padding: 3px 6px; vertical-align: top; text-align: left; }}
th {{ background: #eee; }}
tr {{ break-inside: avoid; }}
blockquote {{ border-left: 3px solid #999; margin: .6em 0; padding: 2px 10px; color: #333; background: #fafafa; }}
hr {{ border: 0; border-top: 1px solid #ddd; margin: 1.2em 0; }}
a {{ color: #1a4f9c; text-decoration: none; }}
"""

EDGE_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    shutil.which("msedge") or "",
    shutil.which("chrome") or "",
    shutil.which("google-chrome") or "",
    shutil.which("chromium") or "",
]


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    src = pathlib.Path(sys.argv[1]).resolve()
    dst = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else src.with_suffix(".pdf")
    browser = next((p for p in EDGE_CANDIDATES if p and os.path.exists(p)), None)
    if not browser:
        sys.exit("no Edge/Chrome found for --print-to-pdf")

    text = src.read_text(encoding="utf-8")
    # Relative links would become file:/// links to this machine in the PDF.
    text = re.sub(r"\[([^\]]+)\]\((?!https?:|#|mailto:)[^)]*\)", r"\1", text)
    body = markdown.markdown(text, extensions=["tables", "fenced_code", "toc"])
    heading = re.search(r"^# (.+)$", text, re.M)
    title = heading.group(1).strip() if heading else src.stem.replace("_", " ")
    # <base> makes the manual's relative image paths resolve from its own folder.
    html = (f'<!doctype html><html><head><meta charset="utf-8">'
            f'<base href="{src.parent.as_uri()}/"><title>{title}</title>'
            f'<style>{CSS}</style></head><body>\n{body}\n</body></html>')

    tmp = pathlib.Path(tempfile.gettempdir()) / f"{src.stem}.print.html"
    tmp.unlink(missing_ok=True)
    tmp.write_text(html, encoding="utf-8")

    dst.unlink(missing_ok=True)
    subprocess.run([browser, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                    f"--print-to-pdf={dst}", tmp.as_uri()],
                   check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    # The Edge launcher can return before the print finishes; wait for the file to settle.
    deadline, last = time.time() + 60, -1
    while time.time() < deadline:
        size = dst.stat().st_size if dst.exists() else -1
        if size > 0 and size == last:
            break
        last = size
        time.sleep(1)
    else:
        sys.exit(f"PDF was not written: {dst}")
    print(f"{src.name} -> {dst} ({dst.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
