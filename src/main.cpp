#include <string>
#include <vector>
#include <math.h>
#include "xtime_l.h"

#include "xusbps_IniRxTx.h"

#include "uuhreserved.h"


// SD card
#include "xsdps.h"
#include "ff.h"
#include <fstream>

using namespace std;

#define CONTROLLER_DEBUG 1

/************ SD card definitions ************/
static FATFS FS_instance;			// File System instance
static FIL file1;					// File instance
int result;						// FRESULT variable
static const char *path = "0:/";	//  string pointer to the logical drive number
char filename[32] = "devices.txt";

/************ Simulated camera image definitions ************/
u32 const imgWidth = 5*1024;
u32 const imgHeight = 3*1024;
u32 roiX=0;
u32 roiY=0;
u32 roiW = imgWidth;
u32 roiH = imgHeight;
u32 binning = 1;
u32 exposure = 1;
u32 const imgSize = imgWidth*imgHeight; // must be multiples of CAMERA_TRANSFER_SIZE
u16 img[imgSize];		// 16bit version
u8 bytesPerPixel = 2;	// 16bit version

u32 CAMERA_TRANSFER_SIZE = 256*512;
u8* buffImg;
static u32 sentImgBytes;
bool sendingImage = false;
u8 txEventCounterEp2=0;

/*** String constants for communicating with MM adapter ***/
string devicename;
string command;
vector<string> words;
vector<string> vals;
string val;
vector<string> emptyvs;

/*
 * Read a line delineated by quotation marks from a file.
 * Example: abcde"efgh"ijk will be read and returned as efgh
 * Returns 0 in case of success, error code otherwise
 */
int read_c_str_from_sd(FIL* file, char* c_str) {

	char* pointer = c_str;
	int result;
	char buf_ch[1];
	UINT nbr=0;	// number of bytes read

	// read to the first quotation mark
	result = f_read(file,(void*)buf_ch,1,&nbr);
	if (result) return result;
	while (nbr==1 && buf_ch[0]!=34) {
		result = f_read(file,(void*)buf_ch,1,&nbr);
	}
	if (nbr!=1) return 20;

	// read between quotation marks
	result = f_read(file,(void*)buf_ch,1,&nbr);
	while (nbr==1 && buf_ch[0]!=34) {
		*c_str = buf_ch[0];
		c_str++;
		result = f_read(file,(void*)buf_ch,1,&nbr);
	}
	if (nbr!=1) return 20;
	*c_str = '\0';
	c_str = pointer;
	return 0;

}

vector<string> SplitStringIntoWords(std::string line, char sep)
{
	// split the string into words
	vector<string> retwords;
	size_t pos1 = 0, pos2 = 0;
	pos1 = (size_t)-1;
	pos2 = line.find(sep, pos1 + 1);
	if (pos2==pos1) {
		retwords.push_back(line);
		return retwords;
	}
	while (pos2 != string::npos)
	{
		retwords.push_back(line.substr(pos1 + 1, pos2 - (pos1 + 1)));
		pos1 = pos2;
		pos2 = line.find(sep, pos1 + 1);
	}
	retwords.push_back(line.substr(pos1 + 1, line.length() - 1));
	return retwords;
}

void make_and_send_output_command(string devicename, string command, int error, vector<string> values) {
	devicename.push_back(uuhwords::sepIn);
	devicename.append(command);
	devicename.push_back(uuhwords::sepIn);
	devicename.append(to_string(error));
	for (size_t cc=0; cc<values.size(); cc++) {
		devicename.push_back(uuhwords::sepWithin);
		devicename.append(values.at(cc));
	}
	devicename.push_back(uuhwords::sepEnd);
	char str[64];
	strcpy(str,devicename.c_str());
	#ifdef CONTROLLER_DEBUG
		xil_printf("Sending string (Ep1):\r\n");
		xil_printf(str);
		xil_printf("\r\n");
	#endif
	SendToEp1((u8*)str, strlen(str));
}

// simulate camera image (16-bit version)
/*void simulate_image() {
	u16 val=0;
	for (u32 ii=posStart; ii<imgHeight; ii++) {
		for (u32 jj=0; jj<imgWidth; jj++) {
			img[ii*imgWidth+jj] = val;
		}
		val++;
	}
	for (u32 ii=0; ii<posStart; ii++) {
		for (u32 jj=0; jj<imgWidth; jj++) {
			img[ii*imgWidth+jj] = val;
		}
		val++;
	}
	posStart++;
	if (posStart==imgHeight) posStart=0;
} */

// simulate camera image (16-bit version)
void simulate_image() {
	XTime curTime;
	XTime_GetTime(&curTime);

	int rho = imgHeight / 4;
	double phi = (2*M_PI*curTime)/0xFFFFFFFF;
	long xc = imgWidth/2 + lround(rho*cos(phi));
	long yc = imgHeight/2 + lround(rho*sin(phi));

	memset(img,0,imgSize*2);
	double val=0;
	for (int hh=yc-25; hh<yc+25; hh++) {
		for (int ww=xc-25; ww<xc+25; ww++) {
			val = ((ww-xc)*(ww-xc)+(hh-yc)*(hh-yc))/(imgHeight/16.0)/(imgHeight/16.0)/2.0;
			val = exp(-val);
			img[hh*imgWidth+ww] = round(65535*val);
		}
	}

/*	double val=0;
	for (u32 hh=0; hh<imgHeight; hh++) {
		for (u32 ww=0; ww<imgWidth; ww++) {
			val = ((ww-xc)*(ww-xc)+(hh-yc)*(hh-yc))/(imgHeight/8.0)/(imgHeight/8.0)/2.0;
			if (ww==imgWidth/2 && hh==imgHeight/2) xil_printf("%d\r\n",val);
			val = exp(-val);
			img[hh*imgWidth+ww] = round(65535*val);
		}
	}
	*/
}

// apply ROI to the simulated image
void apply_roi() {
	if (roiX==0 && roiY==0 && roiW==imgWidth && roiH==imgHeight) return;
	u16* imgTemp;
	imgTemp = img;
	for (u32 jj=roiY; jj<roiH; jj++) {
		memcpy(imgTemp,img+jj*imgWidth+roiX,roiW*bytesPerPixel);
		imgTemp += roiW*bytesPerPixel;
	}
}


int main(void)
{
	int Status;

	// initialize usb2
	Status = SetupUsbDevice(&txEventCounterEp2);
	if (Status != XST_SUCCESS) {
		#ifdef CONTROLLER_DEBUG
			xil_printf("Failed to setup USB2.\r\n");
		#endif
		return XST_FAILURE;
	}

	// mount the sd card
	Status = f_mount(&FS_instance,path, 1);
	if (Status != XST_SUCCESS) {
		#ifdef CONTROLLER_DEBUG
			xil_printf("Failed to mount the SD card.\r\n");
		#endif
		return XST_FAILURE;
	}

	u8* rxBuffEp1;
	u8 lenRxBuffEp1;
	string str;
	char c_str[512];

	// initial state of shutters A and B
	bool sh_open_A = false;

	while (1) {
		ReadFromEp1(&rxBuffEp1, &lenRxBuffEp1);
		if (lenRxBuffEp1!=0) {
			// there is a new command on Ep1
			rxBuffEp1[lenRxBuffEp1] = '\0';
			#ifdef CONTROLLER_DEBUG
				xil_printf("Received string (Ep1):\r\n");
				xil_printf((char*)rxBuffEp1);
				xil_printf("\r\n");
			#endif
			// process the incoming command
	    	str = string((char*)rxBuffEp1);
	    	ResetRxBufferEp1();
	    	str = str.substr(0,str.find_first_of(uuhwords::sepEnd,0));

	    	if (str.size()>0) {
	    		// handle special one-word commands first
	        	if (strcmp(str.c_str(),uuhwords::device_list_start)==0) {
	        		// start passing device descriptions
	        		Status = f_open(&file1, filename, FA_READ);
	        		Status = read_c_str_from_sd(&file1,c_str);
					#ifdef CONTROLLER_DEBUG
	        			xil_printf("Sending string (Ep1):\r\n");
	        			xil_printf(c_str);
	        			xil_printf("\r\n");
					#endif
	        		SendToEp1((u8*)c_str, strlen(c_str));
	        		continue;
	        	} else if (strcmp(str.c_str(),uuhwords::device_list_continue)==0) {
	        		result = read_c_str_from_sd(&file1,c_str);
					#ifdef CONTROLLER_DEBUG
	        			xil_printf("Sending string (Ep1):\r\n");
	        			xil_printf(c_str);
	        			xil_printf("\r\n");
					#endif
	        		SendToEp1((u8*)c_str, strlen(c_str));
	        		if (strcmp(c_str,uuhwords::device_list_end)==0) f_close(&file1);
	        		continue;
	        	}
	        	// handle regular commands
	        	words = SplitStringIntoWords(str,uuhwords::sepOut);
	        	if (words.size()==3) {
	        		devicename = words[0];
	        		command = words[1];
	        		vals = SplitStringIntoWords(words[2],uuhwords::sepWithin);
	        		if (strcmp(devicename.c_str(),"Shutter-A")==0) {
	    				if (strcmp(command.c_str(),"SO")==0) {
	    					if (strcmp(vals[0].c_str(),"0")==0) {
	    						sh_open_A = false;
	    					} else {
	    						sh_open_A = true;
	    					}
	    					vals.clear();
	    					vals.push_back(to_string(sh_open_A));
							make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"SF")==0) {
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"SI")==0) {
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"SS")==0) {
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"DL1")==0) {
	        				sleep(2);
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"DL2")==0) {
	        				vector<string> inivals = vals;
	    					vals.clear();
	    					vals.push_back(to_string(2500));
	        				make_and_send_output_command(devicename,uuhwords::timeout,1,vals);
	        				sleep(2);
	        				make_and_send_output_command(devicename,command,0,inivals);
	    					vals.clear();
	    					vals.push_back(to_string(1000));
	        				make_and_send_output_command(devicename,uuhwords::timeout,0,vals);
	        			} else if (strcmp(command.c_str(),"RB0.1")==0) {
	        				float v = atof(vals[0].c_str());
	        				v = v - 0.1;
	        				vals.clear();
	        				vals.push_back(to_string(v));
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"UBCnot")==0) {
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"OOLF")==0) {
	        				float v = 10.0;
	        				vals.clear();
	        				vals.push_back(to_string(v));
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"OOLI")==0) {
	        				int v = 10.0;
	        				vals.clear();
	        				vals.push_back(to_string(v));
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"OOLS")==0) {
	        				string v("H");
	        				vals.clear();
	        				vals.push_back(v);
	        				make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"SBNR")==0) {
	        				make_and_send_output_command(devicename,command,1,vals);
	        			} else {
	        				make_and_send_output_command(devicename,command,uuherrors::ctr_device_command_not_recognized,emptyvs);
	        			}
	        		}
	        		else if (strcmp(devicename.c_str(),"State-A")==0) {
	    				if (strcmp(command.c_str(),"SST")==0) {
							make_and_send_output_command(devicename,command,0,vals);
	        			} else {
	        				make_and_send_output_command(devicename,command,uuherrors::ctr_device_command_not_recognized,emptyvs);
	        			}
	        		}
	        		else if (strcmp(devicename.c_str(),"Stage-A")==0) {
	    				if (strcmp(command.c_str(),"SP")==0) {
							make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"HM")==0) {
	    					vals.clear();
	    					vals.push_back(to_string(0.0));
							make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"STOP")==0) {
	    					vals.clear();
	    					vals.push_back(to_string(666.666));
							make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"MD")==0) {
							make_and_send_output_command(devicename,command,0,vals);
	        			} else {
	        				make_and_send_output_command(devicename,command,uuherrors::ctr_device_command_not_recognized,emptyvs);
	        			}
	        		}
	        		else if (strcmp(devicename.c_str(),"XYStage-A")==0) {
	    				if (strcmp(command.c_str(),"SP")==0) {
							make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"HM")==0) {
	    					vals.clear();
	    					vals.push_back(to_string(0.0));
	    					vals.push_back(to_string(0.0));
							make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"STOP")==0) {
	    					vals.clear();
	    					vals.push_back(to_string(666.666));
	    					vals.push_back(to_string(777.777));
							make_and_send_output_command(devicename,command,0,vals);
	        			} else if (strcmp(command.c_str(),"MD")==0) {
							make_and_send_output_command(devicename,command,0,vals);
	        			} else {
	        				make_and_send_output_command(devicename,command,uuherrors::ctr_device_command_not_recognized,emptyvs);
	        			}
	        		}
        			else if (strcmp(devicename.c_str(),"Camera-A")==0) {
        				if (strcmp(command.c_str(),"SI")==0) {
        					simulate_image();
        					//apply_roi();
        					//apply_binning();
        					//usleep(exposure*1000);
        					make_and_send_output_command(devicename,command,0,vals);
        				} else if (strcmp(command.c_str(),"GIB")==0) {
        					buffImg = (u8*)img;
        					sentImgBytes = 0;
        					sendingImage = true;
        					// no response is sent here but
        					// image transfer will begin after
        					// input command has been processed
        					// (see usage of sendingImage flag)
        				} else if (strcmp(command.c_str(),"DN")==0) {
        					sleep(2);
        					make_and_send_output_command(devicename,command,0,vals);
        				} else if (strcmp(command.c_str(),"EX")==0) {
        					u32 newExposure = atoi(vals.at(0).c_str());
        					if (newExposure<1 || newExposure>10000) newExposure = exposure;
        					exposure = newExposure;
    						vals.clear();
    						itoa(exposure,c_str,10);
    						vals.push_back(string(c_str));
        					make_and_send_output_command(devicename,command,0,vals);
        				} else if (strcmp(command.c_str(),"SB")==0) {
        					u32 newBinning = atoi(vals.at(0).c_str());
        					if (newBinning<=0 || newBinning>4) newBinning = binning;
		        			roiX = (roiX*binning)/newBinning;
		        			roiY = (roiY*binning)/newBinning;
        					roiW = (roiW*binning)/newBinning;
        					roiH = (roiH*binning)/newBinning;
        					binning = newBinning;
    						vals.clear();
    						itoa(binning,c_str,10);
    						vals.push_back(string(c_str));
        					make_and_send_output_command(devicename,command,0,vals);
        				} else if (strcmp(command.c_str(),"SR")==0) {
        					roiX = atoi(vals.at(0).c_str());
        					roiY = atoi(vals.at(1).c_str());
        					roiW = atoi(vals.at(2).c_str());
        					roiH = atoi(vals.at(3).c_str());
        					if (roiW<=0 || roiW>imgWidth/binning ||
        						roiH<=0 || roiH>imgHeight/binning ||
								roiX<0 || roiX>imgWidth/binning || (roiX+roiW)>imgWidth/binning ||
								roiY<0 || roiY>imgHeight/binning || (roiY+roiH)>imgHeight/binning) {
        						roiX=0;
        						roiY=0;
        						roiW = imgWidth/binning;
        						roiH = imgHeight/binning;
        					}
    						vals.clear();
    						itoa(roiX,c_str,10);
    						vals.push_back(string(c_str));
    						itoa(roiY,c_str,10);
    						vals.push_back(string(c_str));
    						itoa(roiW,c_str,10);
    						vals.push_back(string(c_str));
    						itoa(roiH,c_str,10);
    						vals.push_back(string(c_str));
        					make_and_send_output_command(devicename,command,0,vals);
        				} else if (strcmp(command.c_str(),"CR")==0) {
        					make_and_send_output_command(devicename,command,0,vals);
        				} else if (strcmp(command.c_str(),"TT")==0) {
        					make_and_send_output_command(devicename,command,0,vals);
        				} else if (strcmp(command.c_str(),"TS")==0) {
        					CAMERA_TRANSFER_SIZE = atoi(vals.at(0).c_str());
        					make_and_send_output_command(devicename,command,0,vals);
        				} else {
        					make_and_send_output_command(devicename,command,uuherrors::ctr_device_command_not_recognized,emptyvs);
        				}
        			}
	        		else {
	        			make_and_send_output_command(devicename,command,uuherrors::ctr_device_not_recognized,emptyvs);
	        		}
	        	} else {
	        		devicename = string("Controller");
	        		command = string("ERR");
	        		make_and_send_output_command(devicename,command,uuherrors::ctr_string_not_recognized,emptyvs);
	        	}
	        }
	    }
		// continue sending camera image
    	 if (sendingImage) {
    		 int transferSize;
    		 if (sentImgBytes<roiW*roiH*bytesPerPixel) {
    			 if (roiW*roiH*bytesPerPixel-sentImgBytes>CAMERA_TRANSFER_SIZE) {
    				 transferSize = CAMERA_TRANSFER_SIZE;
    			 } else {
    				 transferSize = roiW*roiH*bytesPerPixel-sentImgBytes;
    			 }
    			 txEventCounterEp2 = 0;
    			 SendToEp2(buffImg, transferSize);
    			 while (txEventCounterEp2<(transferSize+16*1024-1)/(16*1024)) {usleep(10);} // will be reset by interrupt
    			 sentImgBytes += transferSize;
    			 buffImg += transferSize;
    		 } else {
    			 // finished sending the image
    			 sendingImage = false;
    		 }
    	 }
	}

	return XST_SUCCESS;
}

