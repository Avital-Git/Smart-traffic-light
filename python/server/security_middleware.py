"""
Security Middleware
Implements security headers, rate limiting, and request validation.
"""

import time
import logging
from typing import Dict, Tuple
from fastapi import Request, Response
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.responses import JSONResponse

logger = logging.getLogger(__name__)


class RateLimitMiddleware(BaseHTTPMiddleware):
    """Simple in-memory rate limiter by IP address."""
    
    def __init__(self, app, requests_per_minute: int = 60):
        super().__init__(app)
        self.requests_per_minute = requests_per_minute
        self.requests: Dict[str, list] = {}  # IP -> list of request timestamps
    
    async def dispatch(self, request: Request, call_next) -> Response:
        """Check rate limit and continue if allowed."""
        client_ip = self._get_client_ip(request)
        now = time.time()
        
        # Clean old requests (older than 1 minute)
        if client_ip in self.requests:
            self.requests[client_ip] = [
                ts for ts in self.requests[client_ip]
                if now - ts < 60
            ]
        else:
            self.requests[client_ip] = []
        
        # Check if limit exceeded
        if len(self.requests[client_ip]) >= self.requests_per_minute:
            logger.warning(f"Rate limit exceeded for {client_ip}")
            return JSONResponse(
                status_code=429,
                content={"detail": "Too many requests"}
            )
        
        # Record this request
        self.requests[client_ip].append(now)
        
        response = await call_next(request)
        return response
    
    @staticmethod
    def _get_client_ip(request: Request) -> str:
        """Extract client IP from request, accounting for proxies."""
        # Check for X-Forwarded-For header (from proxies)
        if "x-forwarded-for" in request.headers:
            return request.headers["x-forwarded-for"].split(",")[0].strip()
        
        # Fall back to direct connection
        if request.client:
            return request.client.host
        
        return "unknown"


class SecurityHeadersMiddleware(BaseHTTPMiddleware):
    """Add security headers to all responses."""
    
    async def dispatch(self, request: Request, call_next) -> Response:
        response = await call_next(request)
        
        # Prevent clickjacking
        response.headers["X-Frame-Options"] = "DENY"
        
        # Prevent MIME type sniffing
        response.headers["X-Content-Type-Options"] = "nosniff"
        
        # Enable XSS protection (deprecated but still useful)
        response.headers["X-XSS-Protection"] = "1; mode=block"
        
        # Referrer policy
        response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin"
        
        # Content Security Policy - restrictive by default
        response.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: https:; connect-src 'self' ws: wss:; frame-ancestors 'none'"
        
        # Permissions Policy (formerly Feature Policy)
        response.headers["Permissions-Policy"] = "geolocation=(), microphone=(), camera=()"
        
        # HSTS (only in production with HTTPS)
        if request.url.scheme == "https":
            response.headers["Strict-Transport-Security"] = "max-age=31536000; includeSubDomains"
        
        return response


class APIKeyValidationMiddleware(BaseHTTPMiddleware):
    """Validate API key for protected endpoints."""
    
    def __init__(self, app, api_keys: Dict[str, str], enabled: bool = False):
        super().__init__(app)
        self.api_keys = api_keys
        self.enabled = enabled
        # Endpoints that don't require API key
        self.exempt_paths = {"/health", "/docs", "/openapi.json", "/redoc"}
    
    async def dispatch(self, request: Request, call_next) -> Response:
        # Skip validation if disabled or path is exempt
        if not self.enabled or request.url.path in self.exempt_paths:
            return await call_next(request)
        
        # Check for API key
        api_key = request.headers.get("X-API-Key")
        if not api_key:
            logger.warning(f"Request from {self._get_client_ip(request)} missing API key")
            return JSONResponse(
                status_code=401,
                content={"detail": "Missing API key"}
            )
        
        # Validate API key (compare against stored secrets)
        # Note: In production, these should be hashed
        if api_key not in self.api_keys:
            logger.warning(f"Invalid API key from {self._get_client_ip(request)}")
            return JSONResponse(
                status_code=403,
                content={"detail": "Invalid API key"}
            )
        
        return await call_next(request)
    
    @staticmethod
    def _get_client_ip(request: Request) -> str:
        """Extract client IP from request."""
        if "x-forwarded-for" in request.headers:
            return request.headers["x-forwarded-for"].split(",")[0].strip()
        if request.client:
            return request.client.host
        return "unknown"


class RequestLoggingMiddleware(BaseHTTPMiddleware):
    """Log all requests and responses."""
    
    async def dispatch(self, request: Request, call_next) -> Response:
        start_time = time.time()
        
        # Log request
        logger.debug(f"→ {request.method} {request.url.path}")
        
        response = await call_next(request)
        
        # Calculate processing time
        process_time = time.time() - start_time
        
        # Log response
        logger.debug(f"← {response.status_code} {request.url.path} ({process_time:.3f}s)")
        
        # Add response time header
        response.headers["X-Process-Time"] = str(process_time)
        
        return response
