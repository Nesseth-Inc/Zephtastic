/* SPDX-License-Identifier: MIT — shared sunset, wireframe and transport artwork. */
void synth(void)
{
	/* Striped sunset, cyan mountains and a perspective grid. */
	for (int row = -8; row <= 8; row += 3) {
		int rr = 64 - row * row, half = 0;
		while ((half + 1) * (half + 1) <= rr) ++half;
		rect(22 - half, 31 + row, half * 2 + 1, 1, row < -1 ? PEACH : PINK);
	}
	line(4, 43, 52, 43, CYAN); line(4, 43, 17, 35, CYAN);
	line(17, 35, 27, 41, CYAN); line(27, 41, 39, 31, CYAN);
	line(39, 31, 52, 43, CYAN);
	line(8, 51, 28, 43, CYAN); line(48, 51, 28, 43, CYAN);
	line(4, 47, 52, 47, CYAN); line(4, 51, 52, 51, CYAN);
}
void usb_symbol(int x, int y)
{
	line(x+4,y+2,x+4,y+10,CYAN); line(x+2,y+3,x+4,y,CYAN);
	line(x+4,y,x+6,y+3,CYAN); line(x+4,y+6,x+1,y+6,CYAN);
	line(x+1,y+6,x+1,y+4,CYAN); rect(x,y+3,3,2,CYAN);
	line(x+4,y+8,x+7,y+8,CYAN); line(x+7,y+8,x+7,y+5,CYAN);
	rect(x+6,y+4,3,2,CYAN);
}
void ble_symbol(int x, int y)
{
	line(x+4,y,x+4,y+10,CYAN); line(x+4,y,x+7,y+3,CYAN);
	line(x+7,y+3,x+1,y+8,CYAN); line(x+1,y+2,x+7,y+8,CYAN);
	line(x+7,y+8,x+4,y+10,CYAN);
}
void wide_scene(void)
{
	for(int row=-21;row<=21;row+=4) {
		int rr=441-row*row,half=0;while((half+1)*(half+1)<=rr)++half;
		wf(30-half,69+row,half*2+1,2,row<-2?PEACH:PINK);
	}
	wl(8,95,110,95,CYAN);wl(8,95,38,76,CYAN);wl(38,76,54,89,CYAN);
	wl(54,89,75,68,CYAN);wl(75,68,110,95,CYAN);
	for(int i=0;i<5;++i)wl(8+i*25,95,59,112,CYAN);
	for(int y=99;y<=111;y+=4)wl(8,y,110,y,CYAN);
}
void wide_usb(int x,int y)
{
	wl(x+4,y+2,x+4,y+10,CYAN);wl(x+2,y+3,x+4,y,CYAN);wl(x+4,y,x+6,y+3,CYAN);
	wl(x+4,y+6,x+1,y+6,CYAN);wl(x+1,y+6,x+1,y+4,CYAN);wf(x,y+3,3,2,CYAN);
	wl(x+4,y+8,x+7,y+8,CYAN);wl(x+7,y+8,x+7,y+5,CYAN);wf(x+6,y+4,3,2,CYAN);
}
void wide_ble(int x,int y)
{
	wl(x+4,y,x+4,y+10,CYAN);wl(x+4,y,x+7,y+3,CYAN);
	wl(x+7,y+3,x+1,y+8,CYAN);wl(x+1,y+2,x+7,y+8,CYAN);wl(x+7,y+8,x+4,y+10,CYAN);
}
