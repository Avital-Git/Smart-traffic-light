"""
Security Configuration Module
Manages TLS/HTTPS certificates, environment-based configuration, and security policies.
"""

import os
import json
import logging
from pathlib import Path
from typing import Optional, Dict, Any
from enum import Enum
import ssl

logger = logging.getLogger(__name__)


class Environment(str, Enum):
    """Deployment environment types."""
    DEV = "dev"
    STAGING = "staging"
    PROD = "prod"


class SecurityConfig:
    """Centralized security configuration for production hardening."""

    def __init__(self):
        self.env = self._get_environment()
        self.debug = self.env == Environment.DEV
        
        # TLS/HTTPS Configuration
        self.use_ssl = self._get_use_ssl()
        self.ssl_certfile = self._get_ssl_certfile()
        self.ssl_keyfile = self._get_ssl_keyfile()
        self.ssl_ca_certs = self._get_ssl_ca_certs()
        
        # CORS Configuration
        self.cors_origins = self._get_cors_origins()
        
        # Rate Limiting
        self.rate_limit_enabled = self.env != Environment.DEV
        self.rate_limit_requests_per_minute = 100 if self.env == Environment.DEV else 60
        
        # Security Headers
        self.security_headers_enabled = self.env != Environment.DEV
        
        # Emergency Auth
        self.emergency_auth_enabled = True
        
        # Logging
        self.log_level = self._get_log_level()
        self.log_file = self._get_log_file()
        
        # Session/Token
        self.api_key_enabled = self.env == Environment.PROD
        self.api_keys = self._load_api_keys()
        
        logger.info(f"Security Config initialized for {self.env} environment")
        logger.info(f"SSL enabled: {self.use_ssl}")
        logger.info(f"Debug mode: {self.debug}")

    @staticmethod
    def _get_environment() -> Environment:
        """Get environment from ENV variable or default to DEV."""
        env_str = os.getenv("TRAFFIC_ENV", "dev").lower()
        try:
            return Environment(env_str)
        except ValueError:
            logger.warning(f"Invalid TRAFFIC_ENV '{env_str}', defaulting to dev")
            return Environment.DEV

    @staticmethod
    def _get_use_ssl() -> bool:
        """Determine if SSL/TLS should be used."""
        env = os.getenv("TRAFFIC_ENV", "dev").lower()
        if env == "prod":
            return True
        use_ssl = os.getenv("TRAFFIC_USE_SSL", "false").lower()
        return use_ssl in ("true", "1", "yes")

    @staticmethod
    def _get_ssl_certfile() -> Optional[Path]:
        """Get SSL certificate file path."""
        certfile = os.getenv("TRAFFIC_SSL_CERTFILE")
        if not certfile:
            return None
        
        path = Path(certfile)
        if not path.exists():
            logger.warning(f"SSL certificate file not found: {certfile}")
            return None
        
        logger.info(f"Using SSL certificate: {certfile}")
        return path

    @staticmethod
    def _get_ssl_keyfile() -> Optional[Path]:
        """Get SSL private key file path."""
        keyfile = os.getenv("TRAFFIC_SSL_KEYFILE")
        if not keyfile:
            return None
        
        path = Path(keyfile)
        if not path.exists():
            logger.warning(f"SSL key file not found: {keyfile}")
            return None
        
        # Verify key file permissions (should be 600 or similar)
        mode = oct(path.stat().st_mode)[-3:]
        if not os.name == 'nt':  # On non-Windows systems, check permissions
            if int(mode, 8) > 0o600:
                logger.warning(f"SSL key file has permissive permissions: {mode}. Consider restricting to 600.")
        
        logger.info(f"Using SSL key: {keyfile}")
        return path

    @staticmethod
    def _get_ssl_ca_certs() -> Optional[Path]:
        """Get SSL CA certificates file path."""
        ca_certs = os.getenv("TRAFFIC_SSL_CA_CERTS")
        if not ca_certs:
            return None
        
        path = Path(ca_certs)
        if not path.exists():
            logger.warning(f"SSL CA certs file not found: {ca_certs}")
            return None
        
        logger.info(f"Using SSL CA certs: {ca_certs}")
        return path

    @staticmethod
    def _get_cors_origins() -> list:
        """Get CORS allowed origins from environment."""
        env = os.getenv("TRAFFIC_ENV", "dev").lower()
        
        if env == "prod":
            # In production, explicitly list origins
            origins = os.getenv("TRAFFIC_CORS_ORIGINS", "").split(",")
            origins = [o.strip() for o in origins if o.strip()]
            if not origins:
                logger.warning("TRAFFIC_CORS_ORIGINS not set in production, using restrictive defaults")
                origins = ["https://localhost"]
        else:
            # In dev/staging, be more permissive
            origins = os.getenv("TRAFFIC_CORS_ORIGINS", "http://localhost:3000,http://localhost:8000").split(",")
            origins = [o.strip() for o in origins if o.strip()]
        
        logger.info(f"CORS origins: {origins}")
        return origins

    @staticmethod
    def _get_log_level() -> str:
        """Get logging level from environment."""
        env = os.getenv("TRAFFIC_ENV", "dev").lower()
        log_level = os.getenv("TRAFFIC_LOG_LEVEL", "INFO" if env == "prod" else "DEBUG")
        return log_level.upper()

    @staticmethod
    def _get_log_file() -> Optional[Path]:
        """Get log file path if specified."""
        log_file = os.getenv("TRAFFIC_LOG_FILE")
        if not log_file:
            return None
        
        path = Path(log_file)
        path.parent.mkdir(parents=True, exist_ok=True)
        return path

    @staticmethod
    def _load_api_keys() -> Dict[str, str]:
        """Load API keys from environment or file."""
        api_keys_env = os.getenv("TRAFFIC_API_KEYS")
        
        if api_keys_env:
            try:
                # Expect format: "key1:secret1,key2:secret2"
                keys = {}
                for pair in api_keys_env.split(","):
                    if ":" in pair:
                        key, secret = pair.split(":", 1)
                        keys[key.strip()] = secret.strip()
                return keys
            except Exception as e:
                logger.error(f"Failed to parse TRAFFIC_API_KEYS: {e}")
        
        # Try loading from file
        api_keys_file = os.getenv("TRAFFIC_API_KEYS_FILE")
        if api_keys_file:
            try:
                path = Path(api_keys_file)
                if path.exists():
                    with open(path) as f:
                        data = json.load(f)
                        return data.get("api_keys", {})
            except Exception as e:
                logger.error(f"Failed to load API keys from file: {e}")
        
        return {}

    def create_ssl_context(self) -> Optional[ssl.SSLContext]:
        """Create SSL context for HTTPS."""
        if not self.use_ssl or not self.ssl_certfile or not self.ssl_keyfile:
            return None
        
        try:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(
                certfile=str(self.ssl_certfile),
                keyfile=str(self.ssl_keyfile),
                password=None
            )
            
            if self.ssl_ca_certs:
                context.load_verify_locations(cafile=str(self.ssl_ca_certs))
                context.verify_mode = ssl.CERT_OPTIONAL
            
            logger.info("SSL context created successfully")
            return context
        except Exception as e:
            logger.error(f"Failed to create SSL context: {e}")
            raise

    def get_config_dict(self) -> Dict[str, Any]:
        """Get configuration as dictionary for logging/debugging."""
        return {
            "environment": self.env.value,
            "debug": self.debug,
            "use_ssl": self.use_ssl,
            "ssl_certfile": str(self.ssl_certfile) if self.ssl_certfile else None,
            "ssl_keyfile": str(self.ssl_keyfile) if self.ssl_keyfile else None,
            "cors_origins": self.cors_origins,
            "rate_limit_enabled": self.rate_limit_enabled,
            "rate_limit_requests_per_minute": self.rate_limit_requests_per_minute,
            "security_headers_enabled": self.security_headers_enabled,
            "emergency_auth_enabled": self.emergency_auth_enabled,
            "api_key_enabled": self.api_key_enabled,
            "log_level": self.log_level,
            "log_file": str(self.log_file) if self.log_file else None,
        }


# Global security config instance
security_config = SecurityConfig()


def get_security_config() -> SecurityConfig:
    """Get the global security config instance."""
    return security_config
