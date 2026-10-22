A self-signed certificate and its private key, used by the chezdav
tests to serve HTTPS. They are for testing only, and must not be used
anywhere else.

They can be regenerated with:

    openssl req -x509 -newkey rsa:2048 -nodes \
      -keyout key.pem -out cert.pem -days 36500 \
      -subj "/CN=localhost" \
      -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
