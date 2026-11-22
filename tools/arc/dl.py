#!/usr/bin/env python3
"""Minimal downloader used to fetch community tooling (aczarc.py) from moddb.

Uses Python's own OpenSSL (certifi/ssl) because the sandbox curl/schannel path
fails with SEC_E_NO_CREDENTIALS. Not part of the extractor; recon helper only.

Usage: python dl.py <url> <outfile> [referer]
"""
import sys, urllib.request, ssl, os

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    url, out = sys.argv[1], sys.argv[2]
    ref = sys.argv[3] if len(sys.argv) > 3 else None
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    if ref:
        req.add_header("Referer", ref)
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(req, timeout=40, context=ctx) as r:
            data = r.read()
            print("HTTP", r.status, "final_url", r.geturl(), "bytes", len(data))
            with open(out, "wb") as f:
                f.write(data)
    except Exception as e:  # noqa: BLE001 - recon helper, report and move on
        print("FAILED", type(e).__name__, e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
