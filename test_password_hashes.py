import hashlib
import binascii

# From database
salt_hex = "36dd587f0f7958f2192a1506da703831"
password_hash_hex = "ca23e3205e03fad1559ba2b053dcda43a6a8e6014651fccf3a8aef20e366d7d5"

# Convert hex salt to bytes
salt = binascii.unhexlify(salt_hex)

# Test passwords
test_passwords = [
    "admin123",
    "admin",
    "password",
    "test123",
    "123456",
    "admin@123",
    "Admin@123",
    "",
]

print("=" * 70)
print("PASSWORD HASH TESTING:")
print("=" * 70)
print(f"Salt (hex):           {salt_hex}")
print(f"Expected hash (hex):  {password_hash_hex}")
print()

for test_pass in test_passwords:
    # Compute sha256(salt + password)
    combined = salt + test_pass.encode('utf-8')
    computed_hash = hashlib.sha256(combined).hexdigest()
    
    is_match = computed_hash == password_hash_hex
    status = "✓ MATCH!" if is_match else "✗"
    
    print(f"{status:15} Password: '{test_pass:20}' => {computed_hash[:16]}...")

print()
print("=" * 70)
