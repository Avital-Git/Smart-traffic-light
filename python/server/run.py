"""
Server Launcher with Security Configuration
Starts the FastAPI server with TLS/HTTPS support when configured.
"""

import os
import sys
import logging
import uvicorn
from pathlib import Path
import importlib

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)


def get_ssl_context():
    """Load SSL context from environment when cert/key are configured."""
    try:
        from security_config import get_security_config
        
        config = get_security_config()
        if config.use_ssl and config.ssl_certfile and config.ssl_keyfile:
            return config.create_ssl_context()
    except ImportError:
        logger.warning("security_config module not available")
    except Exception as e:
        logger.error(f"Failed to create SSL context: {e}")
    
    return None


def generate_self_signed_cert():
    """Generate self-signed certificate for development."""
    try:
        x509 = importlib.import_module("cryptography.x509")
        name_oid = importlib.import_module("cryptography.x509.oid")
        hashes = importlib.import_module("cryptography.hazmat.primitives.hashes")
        backends = importlib.import_module("cryptography.hazmat.backends")
        asymmetric_rsa = importlib.import_module("cryptography.hazmat.primitives.asymmetric.rsa")
        serialization = importlib.import_module("cryptography.hazmat.primitives.serialization")
        import datetime
        NameOID = name_oid.NameOID
        default_backend = backends.default_backend
        rsa = asymmetric_rsa
        
        cert_dir = Path("certs")
        cert_dir.mkdir(exist_ok=True)
        
        cert_path = cert_dir / "localhost.pem"
        key_path = cert_dir / "localhost.key"
        
        # Check if certs already exist
        if cert_path.exists() and key_path.exists():
            logger.info(f"Using existing self-signed cert: {cert_path}")
            return str(cert_path), str(key_path)
        
        logger.info("Generating self-signed certificate for development...")
        
        # Generate private key
        private_key = rsa.generate_private_key(
            public_exponent=65537,
            key_size=2048,
            backend=default_backend()
        )
        
        # Generate certificate
        subject = issuer = x509.Name([
            x509.NameAttribute(NameOID.COUNTRY_NAME, u"IL"),
            x509.NameAttribute(NameOID.STATE_OR_PROVINCE_NAME, u"Israel"),
            x509.NameAttribute(NameOID.LOCALITY_NAME, u"Beer Sheva"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, u"Smart Traffic"),
            x509.NameAttribute(NameOID.COMMON_NAME, u"localhost"),
        ])
        
        cert = x509.CertificateBuilder().subject_name(
            subject
        ).issuer_name(
            issuer
        ).public_key(
            private_key.public_key()
        ).serial_number(
            x509.random_serial_number()
        ).not_valid_before(
            datetime.datetime.utcnow()
        ).not_valid_after(
            datetime.datetime.utcnow() + datetime.timedelta(days=365)
        ).add_extension(
            x509.SubjectAlternativeName([
                x509.DNSName(u"localhost"),
                x509.DNSName(u"127.0.0.1"),
                x509.DNSName(u"*.localhost"),
            ]),
            critical=False,
        ).sign(private_key, hashes.SHA256(), default_backend())
        
        # Write certificate
        with open(cert_path, "wb") as f:
            f.write(cert.public_bytes(serialization.Encoding.PEM))
        
        # Write key
        with open(key_path, "wb") as f:
            f.write(private_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption()
            ))
        
        logger.info(f"Self-signed cert created: {cert_path}")
        return str(cert_path), str(key_path)
        
    except ImportError:
        logger.warning("cryptography not installed, skipping self-signed cert generation")
        return None, None
    except Exception as e:
        logger.error(f"Failed to generate self-signed cert: {e}")
        return None, None


def main():
    """Start the FastAPI server with appropriate configuration."""
    
    # Determine environment
    env = os.getenv("TRAFFIC_ENV", "dev")
    host = os.getenv("TRAFFIC_HOST", "0.0.0.0")
    port = int(os.getenv("TRAFFIC_PORT", "8000"))
    use_ssl = os.getenv("TRAFFIC_USE_SSL", "false").lower() in ("true", "1", "yes")
    
    logger.info(f"Starting Smart Traffic Server in {env} mode")
    logger.info(f"Binding to {host}:{port}")
    
    # SSL configuration
    ssl_certfile = None
    ssl_keyfile = None
    ssl_context = None
    
    if use_ssl or env == "prod":
        ssl_certfile = os.getenv("TRAFFIC_SSL_CERTFILE")
        ssl_keyfile = os.getenv("TRAFFIC_SSL_KEYFILE")
        
        # Generate self-signed if in dev mode and no certs provided
        if env == "dev" and (not ssl_certfile or not ssl_keyfile):
            try:
                ssl_certfile, ssl_keyfile = generate_self_signed_cert()
            except Exception as e:
                logger.error(f"Failed to generate self-signed cert: {e}")
        
        if ssl_certfile and ssl_keyfile:
            logger.info("SSL/HTTPS enabled")
            ssl_context = get_ssl_context()
    
    # Start server
    try:
        uvicorn.run(
            "app:app",
            host=host,
            port=port,
            reload=env == "dev",
            log_level="debug" if env == "dev" else "info",
            ssl_certfile=ssl_certfile if ssl_context is None else None,
            ssl_keyfile=ssl_keyfile if ssl_context is None else None,
            # WebSocket support
            ws_max_size=65536,
            ws_ping_interval=20,
            ws_ping_pong_timeout=20,
        )
    except Exception as e:
        logger.error(f"Failed to start server: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
