return_to_65:

	ldx #$00
-	lda basicstub,x
	sta $2001,x
	inx
	cpx #$08
	bne -

	lda #133
	sta $01
	lda #$00
	ldx #$E3
	ldy #$00
	ldz #$B3
	map
	eom

	lda #$02
	jsr $ff32

basicstub
	;; BASIC program that deletes itself
	!byte $07, $20, $e4, $07, $a2, $00, $00, $00

