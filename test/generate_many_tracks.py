import re
import subprocess
import sys

# This generates a test file with a large number of audio tracks
# Used to test builds with a custom NGX_VOD_MAX_TRACK_COUNT value
# Requires ffmpeg with flite support (check with `ffplay -f lavfi flite=text="test"`)

def parse_langs():
	blacklist = ['UND', 'ZXX']
	dict = {}
	with open('../vod/languages_x.h') as f:
		for l in f:
			if l.startswith("LANG("):
				lang = [x.strip() for x in l[5:-2].split(",")]
				if lang[0] in blacklist:
					continue

				identifier = lang[3][1:-1]
				name_english = lang[4][1:-1].replace('"', '')
				dict[identifier] = name_english
	return dict

def generate_ffmpeg_command(languages, max_tracks, output_filename):
	pattern = re.compile('[\W_]+')
	c = ['ffmpeg']
	langs = []
	for ident in languages.keys():
		c.extend(['-f', 'lavfi', '-i', 'flite=text="{}",aloop=20:size=3*8000'.format(pattern.sub('', languages[ident]))])
		langs.append(ident)
		if len(langs) == max_tracks:
			break

	c.append('-shortest')

	for i in range(len(langs)):
		c.extend(['-map', '{}:a'.format(i), '-metadata:s:a:{}'.format(i), 'language={}'.format(langs[i])])
	c.append(output_filename)
	return c

if __name__ == '__main__':
	args = sys.argv[1:]
	if len(args) > 0:
		tracks = int(args[0])
	else:
		tracks = 128

	languages = parse_langs()
	command = generate_ffmpeg_command(languages, tracks, "many_tracks.mp4")
	print('command:')
	print(*command)
	print()
	subprocess.run(command)
