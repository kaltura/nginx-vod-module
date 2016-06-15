from construct import *

TS_PACKET_LENGTH = 188
PAT_PID = 0
PES_MARKER = '\0\0\x01'

MIN_AUDIO_STREAM_ID = 0xC0
MAX_AUDIO_STREAM_ID = 0xDF

MIN_VIDEO_STREAM_ID = 0xE0
MAX_VIDEO_STREAM_ID = 0xEF

mpegTsHeader = BitStruct("mpegTsHeader",
	BitField("syncByte", 8),
	Flag("transportErrorIndicator"),
	Flag("payloadUnitStartIndicator"),
	Flag("transportPriority"),
	BitField("PID", 13),
	BitField("scramblingControl", 2),
	Flag("adaptationFieldExist"),
	Flag("containsPayload"),
	BitField("continuityCounter", 4),
	)

mpegTsAdaptationField = BitStruct("mpegTsAdaptationField",
	BitField("adaptationFieldLength", 8),
	Flag("discontinuityIndicator"),
	Flag("randomAccessIndicator"),
	Flag("esPriorityIndicator"),
	Flag("pcrFlag"),
	Flag("opcrFlag"),
	Flag("splicingPointFlag"),
	Flag("transportPrivateDataFlag"),
	Flag("adaptationFieldExtensionFlag"),
	)
	
# PES header
pesHeader = Struct("pesHeader",
	Range(3, 3, UBInt8("prefix")),
	UBInt8("streamId"),
	UBInt16("pesPacketLength")
	)

# PES optional header
pesOptionalHeader = BitStruct("pesOptionalHeader",
	BitField("markerBits", 2),
	BitField("scramblingControls", 2),
	Flag("priority"),
	Flag("dataAlignmentIndicator"),
	Flag("copyright"),
	Flag("originalOrCopy"),
	Flag("ptsFlag"),
	Flag("dtsFlag"),
	Flag("escrFlag"),
	Flag("esRateFlag"),
	Flag("dsmTrickModeFlag"),
	Flag("additionalCopyInfoFlag"),
	Flag("crcFlag"),
	Flag("extensionFlag"),
	BitField("pesHeaderLength", 8),
	)

pcr = BitStruct("pcr",
	BitField("pcr90kHz", 33),
	BitField("padding", 6),
	BitField("pcr27kHz", 9),
	)

pts = BitStruct("pts",
	BitField("pad1", 4),
	BitField("high", 3),
	BitField("marker1", 1),
	BitField("medium", 15),
	BitField("marker2", 1),
	BitField("low", 15),
	BitField("marker3", 1),
	)

def getPts(ptsStruct):
	return (ptsStruct.high << 30) | (ptsStruct.medium << 15) | ptsStruct.low

def setPts(ptsStruct, ptsValue):
	ptsStruct.high = 	((ptsValue >> 30) & 7)
	ptsStruct.medium = 	((ptsValue >> 15) & 0x7FFF)
	ptsStruct.low = 	( ptsValue 		  & 0x7FFF)

pat = BitStruct("pts",
	BitField("pointerField", 8),
	BitField("tableId", 8),
	Flag("sectionSyntaxIndicator"),
	Flag("zero"),
	BitField("reserved1", 2),	# 11
	BitField("reserved2", 2),	# 00
	BitField("sectionLength", 10),
	BitField("tranportStreamId", 16),
	BitField("reserved3", 2),	# 11
	BitField("versionNumber", 5),
	Flag("currentNextIndicator"),
	BitField("sectionNumber", 8),
	BitField("lastSectionNumber", 8),
	)

patEntry = BitStruct("patEntry",
	BitField("programNumber", 16),
	BitField("reserved", 3),			# 111
	BitField("programPID", 13),
	)
	
pmt = BitStruct("pmt",
	BitField("pointerField", 8),
	BitField("tableId", 8),
	Flag("sectionSyntaxIndicator"),
	Flag("zero"),
	BitField("reserved1", 2),	# 11
	BitField("reserved2", 2),	# 00
	BitField("sectionLength", 10),
	BitField("programNumber", 16),
	BitField("reserved3", 2),	# 11
	BitField("versionNumber", 5),
	Flag("currentNextIndicator"),
	BitField("sectionNumber", 8),			# 0
	BitField("lastSectionNumber", 8),		# 0
	BitField("reserved4", 3),
	BitField("pcrPID", 13),
	BitField("reserved5", 4),
	BitField("reserved6", 2),	# 00
	BitField("programInfoLength", 10),
	)

pmtEntry = BitStruct("pmtEntry",
	BitField("streamType", 8),
	BitField("Reserved1", 3),
	BitField("elementaryPID", 13),
	BitField("Reserved2", 4),
	BitField("reserved3", 2),	# 00
	BitField("esInfoLength", 10),
	)
