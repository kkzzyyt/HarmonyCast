import logging

from .constants import LogLevel


def print_log(level: LogLevel, title: str, message: str) -> None:
    levels = {LogLevel.DEBUG: logging.DEBUG, LogLevel.INFO: logging.INFO,
              LogLevel.WARN: logging.WARNING, LogLevel.ERROR: logging.ERROR,
              LogLevel.FATAL: logging.CRITICAL}
    logging.getLogger("HarmonyCast").log(levels[level], "[%s] %s", title, message)
