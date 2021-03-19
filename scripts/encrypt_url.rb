require 'openssl'
require 'base64'

if ARGV.length != 4
  raise "Usage:\n\truby <base url> <encrypted part> <encryption key> <encryption iv>\n"
end

base_url = ARGV[0]
encrypted_part = ARGV[1]
encryption_key = [ARGV[2]].pack("H*")
encryption_iv = [ARGV[3]].pack("H*")

hash_size = 8

signed_url = Digest::MD5.digest(encrypted_part)[0,hash_size] + encrypted_part

cipher = OpenSSL::Cipher::AES.new('256-CBC')
cipher.encrypt
cipher.key = encryption_key
cipher.iv = encryption_iv

encrypted_data = cipher.update(signed_url)
encrypted_data << cipher.final

puts base_url + Base64.strict_encode64(encrypted_data).tr('+/', '-_').gsub(/(\s|=)*$/,'')
