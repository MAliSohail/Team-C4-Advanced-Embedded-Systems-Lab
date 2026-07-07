#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
if [ -f config.env ]; then
  source config.env
fi
source .venv/bin/activate
exec python app.py
