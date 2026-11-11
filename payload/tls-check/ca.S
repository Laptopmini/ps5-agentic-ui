/* The CA bundle, linked in as data (ca_bundle .. ca_bundle_end). */
  .section .rodata
  .global ca_bundle
  .global ca_bundle_end
ca_bundle:
  .incbin "ca-bundle.crt"
ca_bundle_end:
  .byte 0
