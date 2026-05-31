"""validators.py

Validation module for admin intersection management
Prevents SQL injection, XSS, and ensures data integrity
"""

import re
from typing import Tuple, Optional, Dict, Any


class ValidationError(Exception):
    """Custom validation error exception."""
    pass


def validate_string_safe(value: str, field_name: str, max_length: int = 255, allow_hebrew: bool = True) -> str:
    """
    Validate and sanitize string input.
    
    Args:
        value: String to validate
        field_name: Name of field for error messages
        max_length: Maximum allowed length
        allow_hebrew: Whether Hebrew characters are allowed
    
    Returns:
        Sanitized string
    
    Raises:
        ValidationError: If validation fails
    """
    if not isinstance(value, str):
        raise ValidationError(f"{field_name} must be a string")
    
    value = value.strip()
    
    if len(value) == 0:
        raise ValidationError(f"{field_name} cannot be empty")
    
    if len(value) > max_length:
        raise ValidationError(f"{field_name} cannot exceed {max_length} characters")
    
    # Check for potentially dangerous patterns (SQL keywords, script tags)
    dangerous_patterns = [
        r"(?i)(DROP|DELETE|INSERT|UPDATE|TRUNCATE|ALTER|CREATE)\s+(TABLE|DATABASE|SCHEMA)",
        r"(?i)<\s*script",
        r"(?i)javascript:",
        r"--\s*$",  # SQL comment
        r";",  # SQL statement separator (strict)
        r"\0",  # Null byte
    ]
    
    for pattern in dangerous_patterns:
        if re.search(pattern, value):
            raise ValidationError(f"{field_name} contains invalid characters or patterns")
    
    return value


def validate_intersection_code(code: str) -> str:
    """Validate intersection code (alphanumeric, dashes, underscores)."""
    if not isinstance(code, str):
        raise ValidationError("Intersection code must be a string")
    
    code = code.strip().upper()
    
    if len(code) == 0:
        raise ValidationError("Intersection code cannot be empty")
    
    if len(code) > 50:
        raise ValidationError("Intersection code cannot exceed 50 characters")
    
    # Allow alphanumeric, dash, underscore only
    if not re.match(r'^[A-Z0-9_-]+$', code):
        raise ValidationError("Intersection code must contain only alphanumeric characters, dashes, and underscores")
    
    return code


def validate_intersection_name(name: str) -> str:
    """Validate intersection name."""
    return validate_string_safe(name, "Intersection name", max_length=255, allow_hebrew=True)


def validate_city(city: str) -> str:
    """Validate city name."""
    return validate_string_safe(city, "City", max_length=100, allow_hebrew=True)


def validate_region(region: str) -> str:
    """Validate region name."""
    return validate_string_safe(region, "Region", max_length=100, allow_hebrew=True)


def validate_description(description: str) -> str:
    """Validate description field."""
    return validate_string_safe(description, "Description", max_length=1000, allow_hebrew=True)


def validate_coordinate(value: float, coord_name: str) -> float:
    """
    Validate latitude or longitude coordinate.
    
    Args:
        value: Coordinate value
        coord_name: "latitude" or "longitude"
    
    Returns:
        Validated coordinate
    
    Raises:
        ValidationError: If validation fails
    """
    try:
        value = float(value)
    except (ValueError, TypeError):
        raise ValidationError(f"{coord_name} must be a number")
    
    if coord_name.lower() == "latitude":
        if value < -90 or value > 90:
            raise ValidationError("Latitude must be between -90 and 90")
    elif coord_name.lower() == "longitude":
        if value < -180 or value > 180:
            raise ValidationError("Longitude must be between -180 and 180")
    
    return value


def validate_num_cameras(num_cameras: int) -> int:
    """Validate number of cameras."""
    try:
        num_cameras = int(num_cameras)
    except (ValueError, TypeError):
        raise ValidationError("Number of cameras must be an integer")
    
    if num_cameras < 1:
        raise ValidationError("Number of cameras must be at least 1")
    
    if num_cameras > 16:
        raise ValidationError("Number of cameras cannot exceed 16")
    
    return num_cameras


def validate_intersection_data(data: Dict[str, Any]) -> Dict[str, Any]:
    """
    Validate complete intersection data for create/update.
    
    Args:
        data: Dictionary with intersection fields
    
    Returns:
        Validated and sanitized data dictionary
    
    Raises:
        ValidationError: If any field is invalid
    """
    validated = {}
    
    # Required fields
    if 'code' not in data:
        raise ValidationError("Intersection code is required")
    if 'name' not in data:
        raise ValidationError("Intersection name is required")
    if 'latitude' not in data:
        raise ValidationError("Latitude is required")
    if 'longitude' not in data:
        raise ValidationError("Longitude is required")
    
    validated['code'] = validate_intersection_code(data['code'])
    validated['name'] = validate_intersection_name(data['name'])
    validated['latitude'] = validate_coordinate(data['latitude'], "latitude")
    validated['longitude'] = validate_coordinate(data['longitude'], "longitude")
    
    # Optional fields with defaults
    validated['num_cameras'] = validate_num_cameras(
        data.get('num_cameras', 4)
    )
    
    if 'city' in data and data['city']:
        validated['city'] = validate_city(data['city'])
    else:
        validated['city'] = None
    
    if 'region' in data and data['region']:
        validated['region'] = validate_region(data['region'])
    else:
        validated['region'] = None
    
    if 'description' in data and data['description']:
        validated['description'] = validate_description(data['description'])
    else:
        validated['description'] = None
    
    return validated


def validate_phase_id(phase_id: int, max_phases: int = 8) -> int:
    """Validate traffic phase ID."""
    try:
        phase_id = int(phase_id)
    except (ValueError, TypeError):
        raise ValidationError("Phase ID must be an integer")
    
    if phase_id < 0 or phase_id >= max_phases:
        raise ValidationError(f"Phase ID must be between 0 and {max_phases - 1}")
    
    return phase_id


def validate_lane_id(lane_id: int, max_lanes: int = 8) -> int:
    """Validate lane ID."""
    try:
        lane_id = int(lane_id)
    except (ValueError, TypeError):
        raise ValidationError("Lane ID must be an integer")
    
    if lane_id < 0 or lane_id >= max_lanes:
        raise ValidationError(f"Lane ID must be between 0 and {max_lanes - 1}")
    
    return lane_id


def validate_admin_password(password: str, min_length: int = 8) -> str:
    """
    Validate admin password strength.
    
    Args:
        password: Password to validate
        min_length: Minimum password length
    
    Returns:
        Validated password
    
    Raises:
        ValidationError: If password is too weak
    """
    if not isinstance(password, str):
        raise ValidationError("Password must be a string")
    
    if len(password) < min_length:
        raise ValidationError(f"Password must be at least {min_length} characters long")
    
    if len(password) > 128:
        raise ValidationError("Password cannot exceed 128 characters")
    
    # Check for at least one uppercase, one lowercase, one digit
    has_upper = bool(re.search(r'[A-Z]', password))
    has_lower = bool(re.search(r'[a-z]', password))
    has_digit = bool(re.search(r'[0-9]', password))
    
    if not (has_upper and has_lower and has_digit):
        raise ValidationError("Password must contain uppercase, lowercase, and digit characters")
    
    return password


def sanitize_sql_identifier(identifier: str) -> str:
    """
    Sanitize SQL identifier (table name, column name).
    Only allows alphanumeric and underscore.
    """
    if not re.match(r'^[a-zA-Z_][a-zA-Z0-9_]*$', identifier):
        raise ValidationError(f"Invalid SQL identifier: {identifier}")
    return identifier
