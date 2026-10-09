"""Local-only INI configuration for the game's LLM API connection."""
from __future__ import annotations

import configparser
from dataclasses import dataclass, field, replace
import os
from pathlib import Path
import re
import sys
import urllib.parse

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = (Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else ROOT) / "llm.ini"
DEFAULT_URL = "https://api.openai.com/api/v1/chat/completions"
DEFAULT_MODEL = "gpt"


class ConfigError(ValueError):
    pass


@dataclass(frozen=True)
class BridgeConfig:
    enabled: bool = True
    auto_start: bool = True
    channel: str = "v1"
    house: int = -1
    mode: str = "real"


def _read_ini(path):
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    try:
        with Path(path).open("r", encoding="utf-8-sig") as file:
            parser.read_file(file)
    except FileNotFoundError:
        raise ConfigError("llm.ini was not found; copy llm.example.ini and fill authorization") from None
    except (OSError, UnicodeError, configparser.Error):
        raise ConfigError("could not parse llm.ini; check encoding, sections and duplicate options") from None
    if parser.defaults() or set(parser.sections()) - {"llm", "headers", "bridge"} or not parser.has_section("llm"):
        raise ConfigError("llm.ini must contain [llm] and optionally [headers] and [bridge], with no DEFAULT values")
    return parser


def _bridge_settings(parser):
    settings = {}
    if parser.has_section("bridge"):
        for key, value in parser.items("bridge"):
            name = key.lower()
            if name not in BridgeConfig.__dataclass_fields__ or name in settings:
                raise ConfigError("unknown or duplicated option in [bridge]")
            settings[name] = value.strip()
    try:
        for name in ("enabled", "auto_start"):
            if name in settings:
                settings[name] = configparser.ConfigParser.BOOLEAN_STATES[settings[name].lower()]
        if "house" in settings:
            settings["house"] = int(settings["house"])
    except (ValueError, KeyError):
        raise ConfigError("invalid boolean or numeric setting in [bridge]") from None
    result = BridgeConfig(**settings)
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,63}", result.channel) or not -1 <= result.house <= 255:
        raise ConfigError("invalid channel or house in [bridge]")
    if result.mode not in {"real", "mock"}:
        raise ConfigError("bridge mode must be real or mock")
    return result


def load_bridge_config(path=DEFAULT_CONFIG):
    return _bridge_settings(_read_ini(path))


@dataclass(frozen=True)
class APIConfig:
    protocol: str = "chat_completions"
    api_url: str = DEFAULT_URL
    authorization: str = field(default="", repr=False)
    model: str = DEFAULT_MODEL
    reasoning_effort: str = "xhigh"
    strict: bool = False
    tool_choice: str = "forced"
    parallel_tool_calls: bool = False
    max_output_tokens: int = 4096
    timeout: float = 60
    plan_ttl_seconds: int = 30
    interval: float = 8
    headers: dict = field(default_factory=dict, repr=False)


def validate_config(config, require_auth=True):
    if config.protocol not in {"chat_completions", "responses"}:
        raise ConfigError("protocol must be chat_completions or responses")
    try:
        url = urllib.parse.urlsplit(config.api_url)
    except ValueError:
        raise ConfigError("api_url is not a valid HTTPS endpoint") from None
    if (url.scheme != "https" or not url.hostname or url.username or url.password
            or url.query or url.fragment or any(ord(char) <= 32 for char in config.api_url)):
        raise ConfigError("api_url must be a complete HTTPS endpoint without embedded credentials or query parameters")
    if not config.model or len(config.model) > 256 or any(ord(char) < 32 for char in config.model):
        raise ConfigError("model must be a non-empty model ID")
    if config.reasoning_effort not in {"", "none", "minimal", "low", "medium", "high", "xhigh"}:
        raise ConfigError("unsupported reasoning_effort; use empty to omit it")
    if config.tool_choice not in {"forced", "required", "auto"}:
        raise ConfigError("tool_choice must be forced, required or auto")
    if type(config.strict) is not bool or config.parallel_tool_calls is not False:
        raise ConfigError("strict must be boolean and parallel_tool_calls must remain false")
    if type(config.max_output_tokens) is not int or not 256 <= config.max_output_tokens <= 32768:
        raise ConfigError("max_output_tokens must be between 256 and 32768")
    if not 1 <= config.timeout <= 120 or not 1 <= config.interval <= 300:
        raise ConfigError("timeout or interval is outside the supported range")
    if type(config.plan_ttl_seconds) is not int or not 1 <= config.plan_ttl_seconds <= 30:
        raise ConfigError("plan_ttl_seconds must be between 1 and 30 simulation seconds")
    for name, value in config.headers.items():
        if (not re.fullmatch(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+", name)
                or name.lower() in {"authorization", "content-type", "content-length", "host", "connection"}):
            raise ConfigError("invalid or reserved name in [headers]")
        if not isinstance(value, str) or any(not 32 <= ord(char) <= 126 for char in value):
            raise ConfigError("HTTP header values must be single-line ASCII")
    auth = config.authorization
    if any(not 32 <= ord(char) <= 126 for char in auth) or len(auth) > 8192:
        raise ConfigError("authorization must be a single-line ASCII header value")
    placeholder = auth.strip().lower() in {"", "bearer", "basic", "xxxxx", "bearer xxxxx", "your_api_key", "bearer your_api_key"}
    if require_auth and placeholder:
        raise ConfigError("fill authorization in llm.ini before using the real API")
    return config


def load_config(path=DEFAULT_CONFIG, require_auth=True, **overrides):
    parser = _read_ini(path)  # Parser errors never disclose token-bearing lines.
    _bridge_settings(parser)
    settings = {}
    allowed = set(APIConfig.__dataclass_fields__) - {"headers"}
    for key, value in parser.items("llm"):
        name = key.lower()
        if name not in allowed or name in settings:
            raise ConfigError("unknown or duplicated option in [llm]")
        settings[name] = value.strip()
    try:
        for name in ("strict", "parallel_tool_calls"):
            if name in settings:
                settings[name] = configparser.ConfigParser.BOOLEAN_STATES[settings[name].lower()]
        for name in ("max_output_tokens", "plan_ttl_seconds"):
            if name in settings:
                settings[name] = int(settings[name])
        for name in ("timeout", "interval"):
            if name in settings:
                settings[name] = float(settings[name])
    except (ValueError, KeyError):
        raise ConfigError("invalid boolean or numeric setting in [llm]") from None
    headers = dict(parser.items("headers")) if parser.has_section("headers") else {}
    if len({name.lower() for name in headers}) != len(headers):
        raise ConfigError("duplicated HTTP header in [headers]")
    settings["headers"] = headers
    config = APIConfig(**settings)
    config = replace(config, **{name: value for name, value in overrides.items() if value is not None})
    validate_config(config, require_auth=False)
    # Environment fallback applies only to the original OpenAI endpoint, never
    # silently forward an OpenAI key to the new third-party default.
    if (not config.authorization and config.protocol == "responses"
            and urllib.parse.urlsplit(config.api_url).hostname == "api.openai.com"):
        key = os.environ.get("OPENAI_API_KEY", "")
        if key:
            config = replace(config, authorization="Bearer " + key)
    return validate_config(config, require_auth)
