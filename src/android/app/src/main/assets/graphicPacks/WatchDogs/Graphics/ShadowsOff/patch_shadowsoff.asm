[WatchDogs_USA_v0_CascadeShadowsOff]
moduleMatches = 0x7BC6F16F

# CShadowMaskRenderer::AddCascadeShadowsRenderJobsSmoothTransitions(...) comeca em 0x059D9878 (endereco do simbolo).
# Retorna imediatamente com r3 = 0, sem criar os jobs de renderizacao das cascatas de sombra.
0x059D9878 = li r3, 0
0x059D987C = blr
