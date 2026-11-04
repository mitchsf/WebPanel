"""Prove WebPanel 1.4.0 pages are the 1.3.3 pages with the CSS/JS lifted out.

before/<page>.html : 1.3.3 render (inline <style> and <script>)
after/<page>.html  : 1.4.0 render (link + script src + small inline script)
after/wp.css, after/wp.js : the served assets
"""
import re, sys
from pathlib import Path
B, A = Path("before"), Path("after")
css = (A / "wp.css").read_bytes().decode()
js = (A / "wp.js").read_bytes().decode()
ok = True
for bp in sorted(B.glob("*.html")):
    ap = A / bp.name
    old = bp.read_bytes().decode(); new = ap.read_bytes().decode()
    # old: lift <style>…</style> and the single <script>…</script>
    m = re.search(r"<style>(.*?)</style>", old, re.S); old_css = m.group(1)
    old_wo = old[:m.start()] + old[m.end():]
    m = re.search(r"<script>(.*?)</script></body></html>$", old_wo, re.S); old_js = m.group(1)
    old_wo = old_wo[:m.start()] + "</body></html>"
    # new: lift the link tag, the script src tag and the inline script
    m = re.search(r'<link rel="stylesheet" href="/wp\.css\?v=\d+"><style>:root\{--th:(\d+)px;--thumb:(\d+)px\}</style>', new)
    assert m and m.group(1) == "6" and m.group(2) == "22", "slider override missing or not the default"
    new_wo = new[:m.start()] + new[m.end():]
    m = re.search(r'<script src="/wp\.js\?v=\d+"></script><script>(.*?)</script></body></html>$', new_wo, re.S)
    inline_js = m.group(1)
    new_wo = new_wo[:m.start()] + "</body></html>"
    same_frame = (old_wo == new_wo)
    same_css = (old_css == css)
    # old page script == shared js + inline page script (order-insensitive by statement)
    old_stmts = old_js
    recon = js + inline_js
    # the WAIT branch is unconditional in 1.4.0; the per-page old render may lack it
    wait = "if(m.indexOf('::WAIT::')===0){var e=document.querySelector('.saved-inner'),w=m.substring(8);if(e&&w)e.textContent=w;setTimeout(poll,1500);return;}"
    old_norm = old_stmts.replace(wait, "")
    recon_norm = recon.replace(wait, "")
    # scroll-restore moved to the end of wp.js; save() moved after send(): compare as multisets of chunks
    def chunks(s):
        return sorted(filter(None, re.split(r"(?<=[;}])(?=function |try\{|var |\[\{|if\(sf\))", s)))
    same_js = (chunks(old_norm) == chunks(recon_norm))
    status = "OK " if (same_frame and same_css and same_js) else "DIFF"
    ok &= (status == "OK ")
    print(f"{status} {bp.name}: frame {'=' if same_frame else '≠'}  css {'=' if same_css else '≠'}  js {'=' if same_js else '≠'}  "
          f"page {len(old)} -> {len(new)} B (inline script {len(inline_js)} B)")
    if not same_frame:
        import difflib
        for l in list(difflib.unified_diff(old_wo.split("><"), new_wo.split("><"), lineterm="", n=0))[:12]: print("   ", l[:160])
print(f"wp.css {len(css)} B, wp.js {len(js)} B")
sys.exit(0 if ok else 1)
