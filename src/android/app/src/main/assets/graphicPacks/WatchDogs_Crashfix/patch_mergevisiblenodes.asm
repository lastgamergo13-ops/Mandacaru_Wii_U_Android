[WatchDogs_USA_v0_MergeVisibleNodesFix]
moduleMatches = 0x7BC6F16F

# MergeVisibleNodesGPUVisibilityLCDMA(JobScheduler2::CStreamSplitter&) comeca em 0x05B6DFAC (IP do crash 0x05B6E680 - 0x6D4).
# Retorna imediatamente com r3 = 0, sem processar os blocos do Locked Cache.
0x05B6DFAC = li r3, 0
0x05B6DFB0 = blr
