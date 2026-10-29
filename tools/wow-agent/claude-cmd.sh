#!/bin/bash
# Run one worldserver console command through SOAP (UTF-8 safe) and print its output.
# Usage: claude-cmd.sh 'claude party Claude Привіт!'
set -euo pipefail
DIR=${WOW_AGENT_SERVER_DIR:-$(cd -- "$(dirname -- "$0")" && pwd)}
CRED=$(cat "${WOW_SOAP_CREDENTIAL_FILE:-$DIR/claude-soap.cred}")
CMD=$(printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g')
curl -s -m 10 -u "$CRED" -H 'Content-Type: text/xml; charset=utf-8' "${WOW_SOAP_URL:-http://127.0.0.1:7878/}" --data-binary @- <<EOF | python3 -I -c '
import sys, re, html
x = sys.stdin.read()
m = re.search(r"<result>(.*?)</result>", x, re.S) or re.search(r"<faultstring>(.*?)</faultstring>", x, re.S)
print(("" if "<result>" in x else "FAULT ") + html.unescape(m.group(1)).replace("\r", "").strip() if m else x[:300])'
<?xml version="1.0" encoding="utf-8"?>
<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:ns1="urn:AC">
<SOAP-ENV:Body><ns1:executeCommand><command>$CMD</command></ns1:executeCommand></SOAP-ENV:Body>
</SOAP-ENV:Envelope>
EOF
