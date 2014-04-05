#include <time.h>
#include <stdarg.h>
#include <rfb/rfb.h>
#include <rfb/rfbclient.h>

#ifndef LIBVNCSERVER_HAVE_LIBPTHREAD
#error This test need pthread support (otherwise the client blocks the client)
#endif

#define ALL_AT_ONCE
//#define VERY_VERBOSE

MUTEX(frameBufferMutex);

typedef struct { int id; char* str; } encoding_t;
encoding_t testEncodings[]={
	{ rfbEncodingRaw, "raw" },
	{ rfbEncodingRRE, "rre" },
	/* TODO: fix corre */
	/* { rfbEncodingCoRRE, "corre" }, */
	{ rfbEncodingHextile, "hextile" },
#ifdef LIBVNCSERVER_HAVE_LIBZ
	{ rfbEncodingZlib, "zlib" },
	{ rfbEncodingZlibHex, "zlibhex" },
	/* TODO: implement ZRLE decoding */
	/* { rfbEncodingZRLE, "zrle" }, */
#ifdef LIBVNCSERVER_HAVE_LIBJPEG
	{ rfbEncodingTight, "tight" },
#endif
#endif
	{ 0, 0 }
};

#define NUMBER_OF_ENCODINGS_TO_TEST (sizeof(testEncodings)/sizeof(encoding_t)-1)
//#define NUMBER_OF_ENCODINGS_TO_TEST 1

/* Here come the variables/functions to handle the test output */

const int width=400,height=300;
struct { int x1,y1,x2,y2; } lastUpdateRect;
unsigned int statistics[2][NUMBER_OF_ENCODINGS_TO_TEST];
unsigned int totalFailed,totalCount;
unsigned int countGotUpdate;
MUTEX(statisticsMutex);

void initStatistics() {
	memset(statistics[0],0,sizeof(int)*NUMBER_OF_ENCODINGS_TO_TEST);
	memset(statistics[1],0,sizeof(int)*NUMBER_OF_ENCODINGS_TO_TEST);
	totalFailed=totalCount=0;
	lastUpdateRect.x1=0;
	lastUpdateRect.y1=0;
	lastUpdateRect.x2=width;
	lastUpdateRect.y2=height;
	INIT_MUTEX(statisticsMutex);
}

void updateServerStatistics(int x1,int y1,int x2,int y2) {
	LOCK(statisticsMutex);
	countGotUpdate=0;
	lastUpdateRect.x1=x1;
	lastUpdateRect.y1=y1;
	lastUpdateRect.x2=x2;
	lastUpdateRect.y2=y2;
	UNLOCK(statisticsMutex);
}

void updateStatistics(int encodingIndex,rfbBool failed) {
	LOCK(statisticsMutex);
	if(failed) {
		statistics[1][encodingIndex]++;
		totalFailed++;
	}
	statistics[0][encodingIndex]++;
	totalCount++;
	countGotUpdate++;
	UNLOCK(statisticsMutex);
}

	

/* Here begin the functions for the client. They will be called in a
 * pthread. */

/* maxDelta=0 means they are expected to match exactly;
 * maxDelta>0 means that the average difference must be lower than maxDelta */
rfbBool doFramebuffersMatch(rfbScreenInfo* server,rfbClient* client,
		int maxDelta)
{
	int i,j,k;
	unsigned int total=0,diff=0;
	if(server->width!=client->width || server->height!=client->height)
		return FALSE;
	LOCK(frameBufferMutex);
	/* TODO: write unit test for colour transformation, use here, too */
	for(i=0;i<server->width;i++)
		for(j=0;j<server->height;j++)
			for(k=0;k<3/*server->serverFormat.bitsPerPixel/8*/;k++) {
				unsigned char s=server->frameBuffer[k+i*4+j*server->paddedWidthInBytes];
				unsigned char cl=client->frameBuffer[k+i*4+j*client->width*4];
				
				if(maxDelta==0 && s!=cl) {
					UNLOCK(frameBufferMutex);
					return FALSE;
				} else {
					total++;
					diff+=(s>cl?s-cl:cl-s);
				}
			}
	UNLOCK(frameBufferMutex);
	if(maxDelta>0 && diff/total>=maxDelta)
		return FALSE;
	return TRUE;
}

static rfbBool resize(rfbClient* cl) {
	if(cl->frameBuffer)
		free(cl->frameBuffer);
	cl->frameBuffer=(char*)malloc(cl->width*cl->height*cl->format.bitsPerPixel/8);
	SendFramebufferUpdateRequest(cl,0,0,cl->width,cl->height,FALSE);
}

typedef struct clientData {
	int encodingIndex;
	rfbScreenInfo* server;
	char* display;
} clientData;

static void update(rfbClient* client,int x,int y,int w,int h) {
	clientData* cd=(clientData*)client->clientData;
	int maxDelta=0;
	
#ifndef VERY_VERBOSE
	static const char* progress="|/-\\";
	static int counter=0;

	if(++counter>sizeof(progress)) counter=0;
	fprintf(stderr,"%c\r",progress[counter]);
#else
	rfbClientLog("Got update (encoding=%s): (%d,%d)-(%d,%d)\n",
			testEncodings[cd->encodingIndex].str,
			x,y,x+w,y+h);
#endif

	/* only check if this was the last update */
	if(x+w!=lastUpdateRect.x2 || y+h!=lastUpdateRect.y2) {
#ifdef VERY_VERBOSE
		rfbClientLog("Waiting (%d!=%d or %d!=%d)\n",
				x+w,lastUpdateRect.x2,y+h,lastUpdateRect.y2);
#endif
		return;
	}

	if(testEncodings[cd->encodingIndex].id==rfbEncodingTight)
		maxDelta=5;
	
	updateStatistics(cd->encodingIndex,
			!doFramebuffersMatch(cd->server,client,maxDelta));
}

static void* clientLoop(void* data) {
	rfbClient* client=(rfbClient*)data;
	clientData* cd=(clientData*)client->clientData;
	int argc=4;
	char* argv[4]={"client",
		"-encodings", testEncodings[cd->encodingIndex].str,
		cd->display};
	
	
	sleep(1);
	rfbClientLog("Starting client (encoding %s, display %s)\n",
			testEncodings[cd->encodingIndex].str,
			cd->display);
	if(!rfbInitClient(client,&argc,argv)) {
		rfbClientErr("Had problems starting client (encoding %s)\n",
				testEncodings[cd->encodingIndex].str);
		updateStatistics(cd->encodingIndex,TRUE);
		return 0;
	}
	while(1) {
		if(WaitForMessage(client,50)>=0)
			if(!HandleRFBServerMessage(client))
				break;
	}
	free(((clientData*)client->clientData)->display);
	free(client->clientData);
	if(client->frameBuffer)
		free(client->frameBuffer);
	rfbClientCleanup(client);
	return 0;
}

static void startClient(int encodingIndex,rfbScreenInfo* server) {
	rfbClient* client=rfbGetClient(8,3,4);
	clientData* cd;
	pthread_t clientThread;
	
	client->clientData=malloc(sizeof(clientData));
	client->MallocFrameBuffer=resize;
	client->GotFrameBufferUpdate=update;

	cd=(clientData*)client->clientData;
	cd->encodingIndex=encodingIndex;
	cd->server=server;
	cd->display=(char*)malloc(6);
	sprintf(cd->display,":%d",server->port-5900);

	lastUpdateRect.x1=lastUpdateRect.y1=0;
	lastUpdateRect.x2=server->width;
	lastUpdateRect.y2=server->height;

	pthread_create(&clientThread,NULL,clientLoop,(void*)client);
}

/* Here begin the server functions */

static void idle(rfbScreenInfo* server)
{
	int c;
	rfbBool goForward;

	LOCK(statisticsMutex);
#ifdef ALL_AT_ONCE
	goForward=(countGotUpdate==NUMBER_OF_ENCODINGS_TO_TEST);
#else
	goForward=(countGotUpdate==1);
#endif
	/* if(lastUpdateRect.x2==354)
		rfbLog("server checked: countGotUpdate=%d\n",countGotUpdate); */
	UNLOCK(statisticsMutex);
	if(!goForward)
		return;
	countGotUpdate=0;

	LOCK(frameBufferMutex);
	{
		int i,j;
		int x1=(rand()%(server->width-1)),x2=(rand()%(server->width-1)),
		y1=(rand()%(server->height-1)),y2=(rand()%(server->height-1));
		if(x1>x2) { i=x1; x1=x2; x2=i; }
		if(y1>y2) { i=y1; y1=y2; y2=i; }
		x2++; y2++;
		for(c=0;c<3;c++) {
			for(i=x1;i<x2;i++)
				for(j=y1;j<y2;j++)
					server->frameBuffer[i*4+c+j*server->paddedWidthInBytes]=255*(i-x1+j-y1)/(x2-x1+y2-y1);
		}
		rfbMarkRectAsModified(server,x1,y1,x2,y2);

		lastUpdateRect.x1=x1;
		lastUpdateRect.y1=y1;
		lastUpdateRect.x2=x2;
		lastUpdateRect.y2=y2;
#ifdef VERY_VERBOSE
		rfbLog("Sent update (%d,%d)-(%d,%d)\n",x1,y1,x2,y2);
#endif
	}
	UNLOCK(frameBufferMutex);
}

/* log function (to show what messages are from the client) */

void
rfbTestLog(const char *format, ...)
{
	va_list args;
	char buf[256];
	time_t log_clock;

	if(!rfbEnableClientLogging)
		return;

	va_start(args, format);

	time(&log_clock);
	strftime(buf, 255, "%d/%m/%Y %X (client) ", localtime(&log_clock));
	fprintf(stderr,buf);

	vfprintf(stderr, format, args);
	fflush(stderr);

	va_end(args);
}

/* the main function */

int main(int argc,char** argv)
{                                                                
	int i,j;
	time_t t;
	rfbScreenInfoPtr server;

	rfbClientLog=rfbTestLog;
	rfbClientErr=rfbTestLog;

	/* Initialize server */
	server=rfbGetScreen(&argc,argv,width,height,8,3,4);

	server->frameBuffer=malloc(400*300*4);
	for(j=0;j<400*300*4;j++)
		server->frameBuffer[j]=j;
	rfbInitServer(server);
	rfbProcessEvents(server,0);

	initStatistics();

#ifndef ALL_AT_ONCE
	for(i=0;i<NUMBER_OF_ENCODINGS_TO_TEST;i++) {
#else
	/* Initialize clients */
	for(i=0;i<NUMBER_OF_ENCODINGS_TO_TEST;i++)
#endif
		startClient(i,server);

	t=time(0);
	/* test 20 seconds */
	while(time(0)-t<5) {

		idle(server);

		rfbProcessEvents(server,1);
	}
	rfbLog("%d failed, %d received\n",totalFailed,totalCount);
#ifndef ALL_AT_ONCE
	{
		rfbClientPtr cl;
		rfbClientIteratorPtr iter=rfbGetClientIterator(server);
		while((cl=rfbClientIteratorNext(iter)))
			rfbCloseClient(cl);
		rfbReleaseClientIterator(iter);
	}
	}
#endif

	free(server->frameBuffer);
	rfbScreenCleanup(server);

	rfbLog("Statistics:\n");
	for(i=0;i<NUMBER_OF_ENCODINGS_TO_TEST;i++)
		rfbLog("%s encoding: %d failed, %d received\n",
				testEncodings[i].str,statistics[1][i],statistics[0][i]);
	if(totalFailed)
		return 1;
	return(0);
}


