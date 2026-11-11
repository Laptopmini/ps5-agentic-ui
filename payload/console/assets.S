/* Assets linked into the payload as data. */
  .section .rodata
  .global font_ttf
  .global font_ttf_end
  .global emoji_ttf
  .global emoji_ttf_end
  .global symbols2_ttf
  .global symbols2_ttf_end
  .global symbols_ttf
  .global symbols_ttf_end
  .global fixtures_txt
  .global fixtures_txt_end
  .global agent_js
  .global agent_js_end
  .global ca_bundle
  .global ca_bundle_end
  .global pair_html
  .global pair_html_end
  .global nacl_js
  .global nacl_js_end
font_ttf:
  .incbin "font.ttf"
font_ttf_end:
emoji_ttf:
  .incbin "NotoEmoji.ttf"
emoji_ttf_end:
symbols2_ttf:
  .incbin "NotoSansSymbols2.ttf"
symbols2_ttf_end:
symbols_ttf:
  .incbin "NotoSansSymbols.ttf"
symbols_ttf_end:
fixtures_txt:
  .incbin "fixtures.txt"
fixtures_txt_end:
agent_js:
  .incbin "agent.js"
agent_js_end:
ca_bundle:
  .incbin "ca-bundle.crt"
ca_bundle_end:
pair_html:
  .incbin "pair.html"
pair_html_end:
nacl_js:
  .incbin "nacl.min.js"
nacl_js_end:
  .byte 0
