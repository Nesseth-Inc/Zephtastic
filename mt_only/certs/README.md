# MQTT server trust

The ordinary XIAO MQTT gateway installs ISRG Root X1 at its dedicated Zephyr TLS credential tag. TLS checks the configured broker hostname or IP SAN, its certificate chain and its validity period against synchronized time.

The vendored public DER has SHA-256 `96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6`. It was matched to Mozilla’s public trust store and the issuer’s [official certificate](https://letsencrypt.org/certs/isrgrootx1.der). A server must supply a complete supported chain to this root. The firmware profile supports RSA and P-256 certificate keys. P-384 intermediate chains require additional curve support. See [Let’s Encrypt’s chain directory](https://letsencrypt.org/certificates/).

For another broker, give `build-local.py --mqtt-ca-der /path/to/public-root.der` a single self-signed DER CA with certificate-signing usage. This replaces X1 for the image. The build emits the selected public certificate’s SHA-256. Private keys belong on the broker. Server verification remains required for both variants.
